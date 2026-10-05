//! Golden vectors from `reference-tools/storage/generate.rb` (`vectors/storage.json`).
//!
//! Set `CAMPFIRE_STORAGE_VECTORS=/path/to/storage.json` to check against another run (e.g. one
//! generated on a host whose libvips/ffmpeg match the local ones). Processed media is compared
//! byte for byte only when the local libvips/ffmpeg versions match the ones that produced the
//! vectors; otherwise the mismatch is reported and the byte checks are skipped, unless
//! `CAMPFIRE_REQUIRE_MEDIA_VECTORS` is set, as it is in the Dockerfile's toolchain stage that CI
//! tests in: there a mismatch fails.

use std::io::Write;
use std::path::{Path, PathBuf};

use campfire_storage::key::checksum_file;
use campfire_storage::marshal::Value;
use campfire_storage::{Blob, DiskService, Filename, Json, Storage, Variation, disk, disposition, marcel, paths};
use rails_compat::{MessageVerifier, Secrets};
use rusqlite::Connection;
use serde_json::Value as J;

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn vectors_path() -> PathBuf {
    std::env::var_os("CAMPFIRE_STORAGE_VECTORS").map(PathBuf::from).unwrap_or_else(|| repo_root().join("vectors/storage.json"))
}

fn vectors() -> J {
    let path = vectors_path();
    serde_json::from_str(&std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))).unwrap()
}

fn fixture(name: &str) -> PathBuf {
    repo_root().join("reference/test/fixtures/files").join(name)
}

/// `ActiveStorage.verifier`, built as the app builds it (crates/campfire/src/app.rs).
fn verifier() -> MessageVerifier {
    let env = std::fs::read_to_string(repo_root().join("parity/.env.reference")).unwrap();
    let secret_key_base = env.lines().find_map(|l| l.strip_prefix("SECRET_KEY_BASE=")).unwrap();
    rails_compat::app_verifier(&Secrets::new(secret_key_base), "ActiveStorage")
}

fn typed(value: &J) -> Value {
    match value {
        J::Null => Value::Nil,
        J::Bool(b) => Value::Bool(*b),
        J::Number(n) => Value::Int(n.as_i64().unwrap()),
        J::Array(items) => Value::Array(items.iter().map(typed).collect()),
        J::Object(o) if o.contains_key("sym") => Value::Symbol(o["sym"].as_str().unwrap().into()),
        J::Object(o) if o.contains_key("str") => Value::Str(o["str"].as_str().unwrap().into()),
        J::Object(o) => {
            Value::Hash(o["hash"].as_array().unwrap().iter().map(|pair| (pair[0].as_str().unwrap().to_string(), typed(&pair[1]))).collect())
        }
        J::String(_) => panic!("untyped string"),
    }
}

fn variation(value: &J) -> Variation {
    match typed(value) {
        Value::Hash(entries) => Variation::new(entries),
        other => panic!("{other:?}"),
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

fn now() -> jiff::Timestamp {
    "2026-09-26T12:00:00Z".parse().unwrap()
}

#[test]
fn variation_digests_and_keys() {
    let verifier = verifier();
    for v in vectors()["variations"].as_array().unwrap() {
        let variation = variation(&v["typed"]);
        assert_eq!(hex(&variation.marshal()), v["marshal_hex"], "{}", v["inspect"]);
        assert_eq!(variation.digest(), v["digest"], "{}", v["inspect"]);
        assert_eq!(variation.key(&verifier), v["key"], "{}", v["inspect"]);

        let decoded = Variation::decode(&verifier, v["key"].as_str().unwrap(), now()).unwrap();
        assert_eq!(decoded, self::variation(&v["decoded_typed"]), "{}", v["decoded_inspect"]);
        assert_eq!(hex(&decoded.marshal()), v["decoded_marshal_hex"], "{}", v["decoded_inspect"]);
        assert_eq!(decoded.digest(), v["decoded_digest"], "{}", v["decoded_inspect"]);
    }
}

#[test]
fn verifier_messages_and_disk_urls() {
    let verifier = verifier();
    let v = &vectors()["verifier"];
    let expires_at: jiff::Timestamp = "2030-01-02T03:04:05.678Z".parse().unwrap();
    assert_eq!(verifier.generate_raw("\"x\"", Some("p"), Some(expires_at)), v["expiring"]);
    assert_eq!(verifier.verify_raw(v["expiring"].as_str().unwrap(), Some("p"), now()).ok().as_deref(), Some("\"x\""));
    assert!(verifier.verify_raw(v["expiring"].as_str().unwrap(), Some("p"), expires_at).is_err());
    assert!(verifier.verify_raw(v["expiring"].as_str().unwrap(), Some("q"), now()).is_err());

    let service = DiskService::new("/tmp/unused", "local");
    let weird = Filename::new("weird & <name> ünï.png");
    let key = "abcdefghijklmnopqrstuvwxyz12";
    assert_eq!(service.url_path(&verifier, key, None, &weird, Some("image/png"), "inline"), v["disk_url_path"]);
    assert_eq!(service.url_path(&verifier, key, None, &weird, None, "attachment"), v["disk_url_path_nil_type"]);

    let encoded_key = v["disk_url_path"].as_str().unwrap().split('/').nth(4).unwrap();
    let decoded = disk::decode_verified_key(&verifier, encoded_key, now()).unwrap();
    assert_eq!(decoded.key, key);
    assert_eq!(decoded.content_type.as_deref(), Some("image/png"));
    assert_eq!(decoded.service_name, "local");

    let token = v["direct_upload_path"].as_str().unwrap().rsplit('/').next().unwrap();
    let issued: jiff::Timestamp = "2020-01-01T00:00:00Z".parse().unwrap();
    let decoded = disk::decode_verified_token(&verifier, token, issued).unwrap();
    assert_eq!((decoded.content_length, decoded.checksum.as_str()), (42, "abc=="));
}

#[test]
fn marcel_identification() {
    for sample in vectors()["marcel"].as_array().unwrap() {
        let data = match sample["fixture"].as_str() {
            Some(name) => std::fs::read(fixture(name)).unwrap(),
            None => unhex(sample["data_hex"].as_str().unwrap()),
        };
        assert_eq!(
            marcel::identify(&data, sample["name"].as_str(), sample["declared_type"].as_str()),
            sample["content_type"].as_str().unwrap(),
            "{} declared {:?}",
            sample["name"],
            sample["declared_type"]
        );
    }
}

/// `rails_compat::content_disposition` checks the `inline` and `attachment` headers for these names.
#[test]
fn filenames_and_dispositions() {
    for f in vectors()["filenames"].as_array().unwrap() {
        let filename = Filename::from_bytes(&unhex(f["input_hex"].as_str().unwrap()));
        let sanitized = filename.sanitized();
        assert_eq!(sanitized, f["sanitized"].as_str().unwrap());
        // Invalid bytes are replaced on input, so compare base/extension only for valid names.
        if std::str::from_utf8(&unhex(f["input_hex"].as_str().unwrap())).is_ok() {
            assert_eq!(hex(filename.base().as_bytes()), f["base_hex"], "{sanitized}");
            assert_eq!(hex(filename.extension().as_bytes()), f["extension_hex"], "{sanitized}");
        }
        assert_eq!(disposition::escape_path(&sanitized), f["escaped_path"].as_str().unwrap());
    }
}

fn blob_from(row: &J) -> Blob {
    Blob {
        id: row["id"].as_i64().unwrap(),
        key: row["key"].as_str().unwrap().into(),
        filename: Filename::new(row["filename"].as_str().unwrap()),
        content_type: row["content_type"].as_str().map(str::to_string),
        metadata: Json::parse(row["metadata"].as_str().unwrap()).unwrap(),
        service_name: row["service_name"].as_str().unwrap().into(),
        byte_size: row["byte_size"].as_i64().unwrap(),
        checksum: row["checksum"].as_str().map(str::to_string),
        created_at: String::new(),
    }
}

#[test]
fn route_paths() {
    let verifier = verifier();
    let service = DiskService::new("/tmp/unused", "local");
    for m in vectors()["messages"].as_array().unwrap() {
        let blob = blob_from(&m["blob"]);
        assert_eq!(paths::blob_redirect_path(&verifier, &blob, None), m["rails_blob_path"]);
        assert_eq!(paths::blob_redirect_path(&verifier, &blob, Some("attachment")), m["rails_blob_download_path"]);
        assert_eq!(paths::blob_proxy_path(&verifier, &blob, None), m["rails_blob_proxy_path"]);
        assert_eq!(
            paths::verify_signed_blob_id(&verifier, m["rails_blob_path"].as_str().unwrap().split('/').nth(5).unwrap(), now()),
            Some(blob.id)
        );

        // `blob.url` → the disk service URL, with the forced disposition for non-inline types.
        let content_type = blob.content_type();
        let disposition = campfire_storage::content_types::forced_disposition(content_type).unwrap_or("inline");
        let service_path = |disposition: &str| {
            let d = campfire_storage::content_types::forced_disposition(content_type).unwrap_or(disposition);
            format!(
                "http://campfire.test{}",
                service.url_path(
                    &verifier,
                    &blob.key,
                    None,
                    &blob.filename,
                    Some(campfire_storage::content_types::for_serving(content_type)),
                    d
                )
            )
        };
        assert_eq!(service_path(disposition), m["service_url"]);
        assert_eq!(service_path("attachment"), m["service_url_attachment"]);

        if let Some(thumb_path) = m["thumb_path"].as_str() {
            let thumb = variation(&m["variants"][0]["transformations_typed"]);
            assert_eq!(paths::representation_redirect_path(&verifier, &blob, &thumb), thumb_path);
            assert_eq!(paths::representation_proxy_path(&verifier, &blob, &thumb), m["thumb_proxy_path"]);
        }
        if let Some(poster_path) = m["poster_path"].as_str() {
            let poster = Variation::new(vec![
                ("format".into(), Value::Symbol("webp".into())),
                ("resize_to_limit".into(), Value::Array(vec![Value::Int(1200), Value::Int(800)])),
            ]);
            assert_eq!(paths::representation_redirect_path(&verifier, &blob, &poster), poster_path);
            assert_eq!(paths::representation_proxy_path(&verifier, &blob, &poster), m["poster_proxy_path"]);
        }
    }
}

// --- The upload → analyze → variant/preview pipeline -------------------------------------------

const SCHEMA: &str = r#"
CREATE TABLE active_storage_attachments (id integer PRIMARY KEY AUTOINCREMENT NOT NULL, blob_id bigint NOT NULL, created_at datetime(6) NOT NULL, name varchar NOT NULL, record_id bigint NOT NULL, record_type varchar NOT NULL);
CREATE UNIQUE INDEX index_active_storage_attachments_uniqueness ON active_storage_attachments (record_type, record_id, name, blob_id);
CREATE TABLE active_storage_blobs (id integer PRIMARY KEY AUTOINCREMENT NOT NULL, byte_size bigint NOT NULL, checksum varchar, content_type varchar, created_at datetime(6) NOT NULL, filename varchar NOT NULL, key varchar NOT NULL, metadata text, service_name varchar NOT NULL);
CREATE UNIQUE INDEX index_active_storage_blobs_on_key ON active_storage_blobs (key);
CREATE TABLE active_storage_variant_records (id integer PRIMARY KEY AUTOINCREMENT NOT NULL, blob_id bigint NOT NULL, variation_digest varchar NOT NULL);
CREATE UNIQUE INDEX index_active_storage_variant_records_uniqueness ON active_storage_variant_records (blob_id, variation_digest);
"#;

struct Comparison {
    compare_images: bool,
    compare_video: bool,
    mismatches: Vec<String>,
    identical: Vec<String>,
}

impl Comparison {
    fn new(versions: &J) -> Self {
        let local_vips = campfire_storage::vips::version().unwrap();
        let local_ffmpeg = ffmpeg_version();
        let compare_images = versions["libvips"] == local_vips.as_str();
        let compare_video = compare_images && versions["ffmpeg"] == local_ffmpeg.as_str();
        if !compare_images || !compare_video {
            let versions = format!(
                "vectors have libvips {} / {}, local libvips {local_vips} / {local_ffmpeg}",
                versions["libvips"], versions["ffmpeg"]
            );
            assert!(std::env::var_os("CAMPFIRE_REQUIRE_MEDIA_VECTORS").is_none(), "byte comparisons would be skipped: {versions}");
            // Straight to stderr: libtest captures eprintln! from passing tests.
            let _ = writeln!(std::io::stderr(), "note: skipping byte comparisons that depend on versions: {versions}");
        }
        Self { compare_images, compare_video, mismatches: vec![], identical: vec![] }
    }

    /// Row fields that don't depend on processing output always match; checksum, size and
    /// dimensions of processed media only when the versions match.
    fn blob(&mut self, label: &str, actual: &Blob, expected: &J, processed: bool, video: bool) {
        assert_eq!(actual.filename.raw(), expected["filename"], "{label} filename");
        assert_eq!(actual.content_type.as_deref(), expected["content_type"].as_str(), "{label} content_type");
        assert_eq!(actual.service_name, expected["service_name"], "{label} service_name");
        let compare = !processed || if video { self.compare_video } else { self.compare_images };
        if compare {
            assert_eq!(actual.metadata.encode(), expected["metadata"].as_str().unwrap(), "{label} metadata");
            if actual.checksum.as_deref() == expected["checksum"].as_str() && actual.byte_size == expected["byte_size"].as_i64().unwrap() {
                self.identical.push(label.to_string());
            } else {
                self.mismatches.push(format!(
                    "{label}: {} bytes {:?}, expected {} bytes {}",
                    actual.byte_size, actual.checksum, expected["byte_size"], expected["checksum"]
                ));
            }
        }
    }
}

fn ffmpeg_version() -> String {
    let output = std::process::Command::new("ffmpeg").arg("-version").output().unwrap();
    String::from_utf8_lossy(&output.stdout).lines().next().unwrap_or("").trim().to_string()
}

#[test]
fn pipeline_matches_the_reference() {
    let vectors = vectors();
    let files = vectors_path().parent().unwrap().join("storage");
    let root = tempfile::tempdir().unwrap();
    let storage = Storage::new(DiskService::new(root.path(), "local"), verifier());
    let conn = Connection::open_in_memory().unwrap();
    conn.execute_batch(SCHEMA).unwrap();
    let mut comparison = Comparison::new(&vectors["versions"]);

    let check_variant = |comparison: &mut Comparison, conn: &Connection, source: &Blob, v: &J, image: Blob, video: bool| {
        let label = v["label"].as_str().unwrap();
        assert_eq!(variation(&v["transformations_typed"]).digest(), v["variation_digest"], "{label} digest");
        comparison.blob(label, &image, &v["blob"], true, video);
        assert_eq!(storage.path_for(&image), root.path().join(image.key.get(0..2).unwrap()).join(&image.key[2..4]).join(&image.key));
        // The saved reference file is the variant blob's content, and ours is what we recorded.
        let expected = std::fs::read(files.join(v["file"].as_str().unwrap())).unwrap();
        assert_eq!(campfire_storage::key::checksum(&expected), v["blob"]["checksum"].as_str().unwrap(), "{label} vector file");
        let actual = std::fs::read(storage.path_for(&image)).unwrap();
        assert_eq!(campfire_storage::key::checksum(&actual), image.checksum.clone().unwrap(), "{label} stored file");
        let record_id = campfire_storage::blob::find_variant_record(conn, source.id, v["variation_digest"].as_str().unwrap()).unwrap();
        let attached = Blob::attached(conn, "ActiveStorage::VariantRecord", record_id.unwrap(), "image").unwrap();
        assert_eq!(attached.map(|b| b.id), Some(image.id), "{label} variant record attachment");
    };

    for m in vectors["messages"].as_array().unwrap() {
        let name = m["fixture"].as_str().unwrap();
        let data = std::fs::read(fixture(name)).unwrap();
        let mut blob = storage.create_and_upload(&conn, &data, Filename::new(name), m["declared_type"].as_str(), now()).unwrap();
        campfire_storage::blob::insert_attachment(&conn, "attachment", "Message", 1, blob.id, now()).unwrap();
        storage.analyze(&conn, &mut blob).unwrap();
        comparison.blob(name, &blob, &m["blob"], false, false);
        assert_eq!(blob.is_variable(), m["variable"].as_bool().unwrap(), "{name} variable?");
        assert_eq!(blob.is_previewable(), m["previewable"].as_bool().unwrap(), "{name} previewable?");

        if blob.is_video() {
            let preview_image = storage.preview_image(&conn, &blob, now()).unwrap();
            comparison.blob(&format!("{name} preview_image"), &preview_image, &m["preview_image"]["blob"], true, true);
            for v in m["variants"].as_array().unwrap() {
                let image = storage.process_preview(&conn, &blob, &variation(&v["transformations_typed"]), now()).unwrap();
                check_variant(&mut comparison, &conn, &preview_image, v, image, true);
            }
        } else if blob.is_variable() {
            let thumb = Variation::resize_to_limit(1200, 800, None);
            let variation = storage.variation_for(&blob, &thumb).unwrap();
            let v = &m["variants"][0];
            assert_eq!(variation, self::variation(&v["transformations_typed"]));
            let image = storage.process_variant(&conn, &blob, &variation, now()).unwrap();
            // Processing again reuses the variant record.
            assert_eq!(storage.process_variant(&conn, &blob, &variation, now()).unwrap().id, image.id);
            check_variant(&mut comparison, &conn, &blob, v, image, false);
        }
    }

    let named =
        [("avatars", Variation::resize_to_limit(512, 512, Some("webp"))), ("logos", Variation::resize_to_limit(512, 512, Some("png")))];
    for (kind, first) in named {
        for entry in vectors[kind].as_array().unwrap() {
            let row = &entry["blob"];
            let data = std::fs::read(fixture(entry["fixture"].as_str().unwrap())).unwrap();
            let mut blob = storage
                .create_and_upload(&conn, &data, Filename::new(row["filename"].as_str().unwrap()), row["content_type"].as_str(), now())
                .unwrap();
            storage.analyze(&conn, &mut blob).unwrap();
            comparison.blob(row["filename"].as_str().unwrap(), &blob, row, false, false);
            for (i, v) in entry["variants"].as_array().unwrap().iter().enumerate() {
                let transformations = if i == 0 { first.clone() } else { Variation::resize_to_limit(192, 192, Some("png")) };
                let variation = storage.variation_for(&blob, &transformations).unwrap();
                assert_eq!(variation, self::variation(&v["transformations_typed"]), "{}", v["label"]);
                let image = storage.process_variant(&conn, &blob, &variation, now()).unwrap();
                check_variant(&mut comparison, &conn, &blob, v, image, false);
            }
        }
    }

    eprintln!("byte-identical: {:?}", comparison.identical);
    assert!(comparison.mismatches.is_empty(), "not byte-identical:\n{}", comparison.mismatches.join("\n"));
}

#[test]
fn staging_a_file_unfurls_it_as_its_bytes_would() {
    let root = tempfile::tempdir().unwrap();
    let storage = Storage::new(DiskService::new(root.path(), "local"), verifier());
    for m in vectors()["messages"].as_array().unwrap() {
        let name = m["fixture"].as_str().unwrap();
        let declared = m["declared_type"].as_str();
        let from_file = storage.stage_file(&fixture(name), Filename::new(name), declared).unwrap();
        let from_bytes = storage.stage_bytes(&std::fs::read(fixture(name)).unwrap(), Filename::new(name), declared).unwrap();
        let (a, b) = (from_file.blob(), from_bytes.blob());
        assert_eq!((&a.content_type, &a.checksum, a.byte_size), (&b.content_type, &b.checksum, b.byte_size), "{name}");
        assert_eq!(std::fs::read(storage.service.path_for(&a.key)).unwrap(), std::fs::read(fixture(name)).unwrap(), "{name}");
        // Staging doesn't read the copies back, so check here what `DiskService#upload` would.
        for staged in [a, b] {
            assert_eq!(checksum_file(&storage.service.path_for(&staged.key)).unwrap(), staged.checksum, "{name}");
        }
    }
}

#[test]
fn a_staged_file_is_deleted_unless_kept() {
    let root = tempfile::tempdir().unwrap();
    let storage = Storage::new(DiskService::new(root.path(), "local"), verifier());
    let dropped = storage.stage_bytes(b"dropped", Filename::new("a.txt"), None).unwrap();
    let dropped_path = storage.service.path_for(&dropped.blob().key);
    assert!(dropped_path.exists());
    drop(dropped);
    assert!(!dropped_path.exists());

    let kept = storage.stage_bytes(b"kept", Filename::new("b.txt"), None).unwrap();
    let kept_path = storage.service.path_for(&kept.blob().key);
    kept.keep();
    assert!(kept_path.exists());
}

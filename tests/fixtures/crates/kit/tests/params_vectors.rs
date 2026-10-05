//! Differential check of the params parser against Rails' `ParamBuilder.from_query_string`,
//! using vectors generated in the reference container by `params_vectors.rb`.

#[test]
fn query_strings_parse_like_rails() {
    let vectors: Vec<serde_json::Value> = serde_json::from_str(include_str!("params_vectors.json")).expect("params_vectors.json");
    let mut failures = Vec::new();
    for vector in &vectors {
        let input = vector["input"].as_str().unwrap();
        let actual = campfire_kit::parse_nested(input);
        if actual != vector["output"] {
            failures.push(format!("{input:?}\n  rails: {}\n  ours:  {actual}", vector["output"]));
        }
    }
    assert!(failures.is_empty(), "{} of {} differ:\n{}", failures.len(), vectors.len(), failures.join("\n"));
}

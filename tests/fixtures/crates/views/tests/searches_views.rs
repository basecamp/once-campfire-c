//! DOM parity of the search views with the reference app (goldens in tests/golden/b).

mod messages_support;

use askama::Template;
use campfire_views::searches::{self, IndexView};
use messages_support::golden;

#[test]
fn index_with_results() {
    let g = golden("searches_index");
    let index: IndexView = g.input();
    g.assert_dom(&g.render(|ctx| searches::Index { ctx, index: &index }.render().unwrap()));
}

#[test]
fn index_without_query() {
    let g = golden("searches_index_empty");
    let index: IndexView = g.input();
    g.assert_dom(&g.render(|ctx| searches::Index { ctx, index: &index }.render().unwrap()));
}

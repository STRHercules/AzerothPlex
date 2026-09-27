#include "../NativeUi.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    wxl_ui::NativeUiState state;

    wxl_plex::PlexEvent login{wxl_plex::PlexEvent::Kind::LoginSucceeded};
    state.Apply(login);
    CHECK(state.view == wxl_ui::View::Home);

    wxl_plex::PlexEvent error{wxl_plex::PlexEvent::Kind::Error};
    error.message = "stream failed";
    state.Apply(error);
    CHECK(state.view == wxl_ui::View::Player);
    CHECK(state.error == "stream failed");

    wxl_plex::PlexEvent watchlist{wxl_plex::PlexEvent::Kind::Watchlist};
    wxl_plex::PlexItem watchlisted;
    watchlisted.ratingKey = "10";
    watchlisted.guid = "plex://movie/test";
    watchlisted.title = "Watchlisted";
    watchlisted.type = "movie";
    watchlist.items.push_back(watchlisted);
    state.Apply(watchlist);
    CHECK(state.watchlist.size() == 1);

    wxl_plex::PlexEvent children{wxl_plex::PlexEvent::Kind::Children};
    wxl_plex::PlexItem season;
    season.ratingKey = "11";
    season.title = "Season 1";
    season.type = "season";
    children.items.push_back(season);
    state.Apply(children);
    CHECK(state.items.size() == 1);
    CHECK(state.items[0].type == "season");
    return 0;
}

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
    return 0;
}

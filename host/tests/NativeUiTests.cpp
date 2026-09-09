#include "../NativeUi.hpp"

#include <cassert>

int main()
{
    wxl_ui::NativeUiState state;

    wxl_plex::PlexEvent login{wxl_plex::PlexEvent::Kind::LoginSucceeded};
    state.Apply(login);
    assert(state.view == wxl_ui::View::Home);

    wxl_plex::PlexEvent error{wxl_plex::PlexEvent::Kind::Error};
    error.message = "stream failed";
    state.Apply(error);
    assert(state.view == wxl_ui::View::Player);
    assert(state.error == "stream failed");
    return 0;
}

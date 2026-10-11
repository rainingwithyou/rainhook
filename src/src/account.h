#pragma once
// Account reset — wipes the local login/session state so the game returns to
// its login page.
//
// This runs inside the game's own process, as the game's own UID, so it can
// reach both the external files directory and the private data directory
// (/data/data/<pkg>/) without root. The private directory is where the
// session actually lives; the external login_data/ is the cache the game
// consults first.
//
// What this does NOT do: remove a server-side ban. A ban is attached to the
// account on the publisher's servers and follows the account wherever it is
// signed in. Clearing local state gets you back to the login page so you can
// sign in as someone else — it does not un-ban anyone.

namespace account {

// Log what account-related state exists, without removing anything. Called at
// boot so the log always carries a picture of the device's state.
void Diagnose();

// Remove the local login/session state. Returns true when something was
// actually removed (so the UI can say so). The game must be restarted
// afterwards — it holds the old session in memory.
bool Reset();

// Human-readable outcome of the last Reset(), for the menu. Always valid.
const char* LastResult();

}  // namespace account

#pragma once
// Account reset — resets the stored guest identity and wipes the local
// login/session state, so the game returns to its login page as a fresh guest.
//
// This runs inside the game's own process, as the game's own UID, so it can
// reach both the external files directory and the private data directory
// (/data/data/<pkg>/) without root.
//
// The method mirrors the reference implementation's "Reset Guest" feature:
//
//   1. Every device-identity key in the Unity PlayerPrefs XML is replaced
//      with a freshly generated value. The login SDK persists its device
//      identity there under the "JsonDeviceID" / "__Java_JsonDeviceID__" key
//      family, and the guest account is keyed to that stored value — the SDK
//      trusts a present value over re-deriving one from hardware identifiers.
//   2. The login caches (login_data/, newbieofflineData/, SDK identity prefs)
//      are removed so nothing stale survives the swap.
//   3. The process SIGKILLs itself. Unity holds PlayerPrefs in memory and
//      flushes them on a clean exit — a normal quit would write the old
//      identity straight back over the surgery.
//
// A full backup of everything touched lands in
// files/rain_account_backup/<timestamp>/ before any change is made.
//
// What this does NOT do: remove a server-side ban. A ban is attached to the
// account on the publisher's servers. A fresh guest identity reaches the
// login page as a different account — it does not un-ban anyone.

namespace account {

// Log what account-related state exists, without removing anything. Called at
// boot so the log always carries a picture of the device's state.
void Diagnose();

// Reset the guest identity and wipe the local login/session state. The
// process is SIGKILLed at the end (never returns on success) so the old
// identity cannot be flushed back by Unity's exit handler.
bool Reset();

// Human-readable outcome of the last Reset(), for the menu. Always valid.
const char* LastResult();

}  // namespace account

// Software Keyboard intentionally has no HLE override.
//
// The recompiled NintendoSDK implementation of nn::swkbd::ShowKeyboard now
// runs unchanged and talks to runtime/services/am.c through Horizon's
// ILibraryAppletCreator / ILibraryAppletAccessor protocol. Keeping this file
// (rather than deleting it from the wildcard build) documents that the absence
// of a ShowKeyboard HLE binding is deliberate.
//
// The host only provides the system-applet frontend. Game-visible result,
// cancellation, UTF-8/UTF-16 handling and TextCheck travel through the same
// storage/event IPC path the SDK expects.

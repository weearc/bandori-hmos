# Host native-library directory mapping

`package_metadata_set_host_native_library_dir(vm,path)` accepts an explicit host-supplied directory only for the current metadata VM. It canonicalizes with realpath and checks stat/S_ISDIR, then copies into host-owned bounded storage. Invalid input preserves the previous mapping; NULL clears it. vm_start resets all state.

ApplicationInfo.nativeLibraryDir stays null without configuration. When configured, it is a Java String containing the actual host path; OOM is reported. This is a compatibility-layer storage mapping, not a claim that an Android PackageManager reported that path. The caller must validate which original libraries are packaged there; directory existence does not prove completeness or identity.

The 72ea runner has not configured or device-tested this new setter. Future UnityPlayer constructor probes must pass the observed HAP library directory and verify the guest field before relying on it. The default-null metadata selftest remains valid. Root corrected the initial clear operation's multi-character literal before integration.

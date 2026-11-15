"""Build an isolated TLS-enabled SDK candidate from the preserved v30 source."""
from pathlib import Path
import hashlib
import os
import json
import shutil
import subprocess
import zipfile
import sys


root = Path(__file__).resolve().parent
workspace = root.parent
base = root / 'preserved-template'
out = root / 'build' / os.environ.get('TLS_CANDIDATE_DIR', 'integrated-login-tls-next')
candidate_label = os.environ.get('TLS_CANDIDATE_LABEL', out.name)
source = out / 'source'
pcre_archive = workspace/'sdk-init-next/vendor/build-pcre2-16/libpcre2-16.a'
if not pcre_archive.is_file():
    subprocess.run([sys.executable,str(workspace/'sdk-init-next/build_pcre2.py')],check=True)
base_manifest = json.loads((base / 'candidate.json').read_text(encoding='utf-8'))
base_library = base / 'libsdktrace.so'
base_hash = hashlib.sha256(base_library.read_bytes()).hexdigest()
if base_hash != base_manifest['sha256']:
    raise SystemExit(f'preserved v30 candidate hash mismatch: {base_hash}')
if out.exists():
    raise SystemExit(f'refusing to overwrite existing candidate directory: {out}')

sdk = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native')
compiler = sdk / 'llvm/bin/clang.exe'
nm = sdk / 'llvm/bin/llvm-nm.exe'
mbed_dir = workspace / 'tls-java-next/native/build'
required_archives = [mbed_dir / f'lib{name}.a' for name in ('mbedtls', 'mbedx509', 'mbedcrypto')]
client = workspace / 'tls-java-next/native/tls_client.c'
cert_runtime = workspace / 'tls-java-next/java/cert_runtime.c'
ssl_runtime = workspace / 'tls-java-next/java/ssl_runtime.c'
sqlite = workspace / 'sqlite-backend-next/build/libgame_sqlite.so'
required_inputs = [
    workspace / 'file-directory-next/file_runtime.h',
    workspace / "sdk-runtime-spike/task_step_profile.inc",
    *[workspace / 'sdk-runtime-spike' / name for name in ('okio_buffer_read.inc','okio_buffer_read_selftest.inc','okio_response_read.inc','okio_response_read_selftest.inc')],
    workspace / "sdk-runtime-spike/okio_buffer_selftest.inc",
    workspace / "sdk-runtime-spike/byte_array_input_stream_selftest.inc",
    workspace / "sdk-runtime-spike/input_stream_reader_selftest.inc",
    workspace / "sdk-runtime-spike/jni_throwable_pressure_selftest.inc",
    compiler, nm, sdk / 'sysroot', sqlite, *required_archives,
    workspace / 'tls-java-next/analysis/SSLUtils.smali.txt',
    base / 'source',
    workspace / 'tls-transport-next/vendor/mbedtls-3.6.7/include',
    *[workspace / 'tls-java-next/java' / name for name in (
        'socket_runtime.c', 'socket_runtime.h', 'socket_shared.h',
        'cert_runtime.c', 'cert_runtime.h', 'exception_hierarchy.c',
        'exception_hierarchy.h', 'ssl_runtime.c', 'ssl_runtime.h',
        'tls_loopback_selftest.c', 'tls_loopback_selftest.h',
        'tls_loopback_fixtures.h')],
    *[workspace / 'surface-runtime-next' / name for name in (
        'surface_runtime.c', 'surface_runtime.h')],
    *[workspace / 'native-jni-next' / name for name in (
        'native_window_jni.c', 'native_window_jni.h')],
    *[workspace / 'tls-java-next/native' / name for name in (
        'tls_client.c', 'tls_client.h', 'tls_mbedtls_user_config.h')],
    *[workspace / 'tls-trust-next' / name for name in (
        'trust.c', 'trust.h', 'fixture_selftest.c', 'fixture_selftest.h',
        'fixture_der.h')],
    *[workspace / 'cert-objects-next' / name for name in (
        'cert_objects.c', 'cert_objects.h', 'cert_selftest.c',
        'cert_selftest.h', 'expected_spki.h')],
    *[workspace / 'application-lifecycle-next' / name for name in (
        'android_basics.c', 'android_basics.h', 'android_basics_test.c','math_interpolator_runtime.inc',
        'activity_runtime.c', 'activity_runtime.h', 'view_state_runtime.c', 'view_state_runtime.h', 'window_attributes.c', 'window_attributes.h', 'display_runtime.c', 'display_runtime.h', 'surface_view_runtime.c', 'surface_view_runtime.h', 'theme_runtime.c', 'theme_runtime.h', 'layout_params_runtime.c', 'layout_params_runtime.h', 'unity_view_mount.c', 'unity_view_mount.h', 'application_lifecycle.c',
        'application_lifecycle.h')],
    *[workspace / 'sdk-init-next' / name for name in (
        'file_runtime.c', 'prefs_runtime.c', 'string_runtime.c',
        'reflection_runtime.c', 'platform_runtime.c', 'java_basics.c',
        'apk_resource_table.inc', 'unity_resource_strings.inc',
        'io_runtime.c', 'io_runtime.h', 'lrucache_runtime.c',
        'lrucache_runtime.h', 'latch_runtime.c', 'latch_runtime.h',
        'executor_extras.c', 'executor_extras.h', 'thread_extras.c',
        'executor_runtime.c', 'collections.c', 'collections_test.c',
        'thread_extras.h', 'thread_runtime.c', 'sqlite_runtime.c',
        'sqlite_runtime.h', 'sdk_init.c', 'sdk_init.h',
        'bundle_runtime.c', 'bundle_runtime.h',
        'content_runtime.c', 'content_runtime.h', 'settings_compat.inc',
        'network_runtime.c', 'network_runtime.h', 'notification_services.inc',
        'intent_runtime.c', 'intent_runtime.h', 'intent_launch_host.h',
        'uri_runtime.c', 'uri_runtime.h', 'java_log_runtime.c')],
    *[workspace / 'list-views-next' / name for name in (
        'list_views.c', 'list_views.h')],
    workspace / 'asset-reader-next/asset_reader.c',
    workspace / 'native-elf-loader-next/native_asset_bridge.c',
    workspace / 'native-elf-loader-next/native_asset_bridge.h',
    workspace / 'native-compat/assets.c',
    workspace / 'native-compat/assets.h',
    workspace / 'native-compat/native_asset_backend_names.h',
    workspace / 'core-object-next/object_runtime.c',
    workspace / 'set-runtime-next/set_runtime.c',
    workspace / 'set-runtime-next/set_runtime.h',
    workspace / 'throwable-reference-next/throwable_reference.c',
    workspace / 'json-runtime-next/json_runtime.c',
    workspace / 'json-runtime-next/json_runtime.h',
    workspace / 'package-metadata-next/reviewed_metadata.c',
    workspace / 'package-metadata-next/package_metadata.h',
    workspace / 'package-metadata-next/metadata_values.inc',
    workspace / 'sdk-init-next/runtime_extra.inc',
    workspace / 'sdk-init-next/runtime_extra_test.inc',
    workspace / 'native-jni-next/jni_forname_selftest.c',
    workspace / 'native-jni-next/jni_activity_selftest.c',
    workspace / 'sdk-init-next/dvm.c',
    *[workspace / 'native-elf-loader-next' / name for name in ('task6f_download_preflight.inc','task6f_ugui_observer.inc','task6k_title_observer.inc')],
    *[workspace / 'runtime-contract-next' / name for name in (
        'runtime_contract.c', 'runtime_contract.h')],
    *[workspace / 'signature-runtime-next' / name for name in (
        'apk_signature_runtime.c', 'apk_signature_runtime.h')],
    *[workspace / 'native-loader-next' / name for name in (
        'native_probe.c', 'native_probe.h')],
    *[workspace / 'native-jni-next' / name for name in (
        'jni_abi.h', 'jni_abi_stubs.h', 'jni_util.c', 'jni_util.h',
        'jni_handles.c', 'jni_handles.h', 'jni_bridge.c', 'jni_bridge.h',
        'native_loader.c', 'native_loader.h')],
    *[workspace / 'native-elf-loader-next' / name for name in (
        'elf_loader.c', 'elf_loader.h', 'elf_loader_probe.c', 'elf_loader_probe.h',
        'android_sensor_shim.c', 'android_sensor_shim.h',
        'android_looper_shim.c', 'android_looper_shim.h',
        'android_property_shim.c', 'android_property_shim.h',
        'task6m_net_probe.inc', 'android_netdb_shim.h', 'android_fdset_shim.c', 'android_fdset_shim.h', 'android_dlfcn.c', 'android_dlfcn.h', 'android_dlfcn_probe.c', 'android_dlfcn_probe.h', 'bionic_ctype_data.h', 'bionic_media_keys.inc', 'android_sysconf_map.inc', 'android_opensles_ids.inc')],
    *[workspace / 'native-window-next' / name for name in ('window_runtime.c','window_runtime.h','egl_window_runtime.c','egl_window_runtime.h')],
    *[workspace / 'native-elf-loader-next' / name for name in ('unity_constructor_probe.c','unity_constructor_probe.h','unity_jni_probe.c','unity_jni_probe.h','il2cpp_probe.c','il2cpp_probe.h','android_semaphore_shim.c','android_semaphore_shim.h','android_signal_shim.c','android_signal_shim.h','android_abi_probe.c','android_abi_probe.h','android_stdio_shim.c','android_stdio_shim.h','android_stdio_probe.c','android_stdio_probe.h','native_main_load_probe.c','native_main_load_probe.h','unity_player_probe.c','unity_player_probe.h','native_fault_probe.c','native_fault_probe.h')],
    *[workspace / 'native-media-next' / name for name in ('media_format.c','media_format.h','media_extractor.c','media_extractor.h','media_backend_probe.c','media_backend_probe.h','media_codec.c','media_codec.h','media_codec_probe.c','media_codec_probe.h')],
    *[workspace / 'device-runtime-next' / name for name in (
        'device_runtime.c', 'device_runtime.h')],
    *[workspace / 'observable-runtime-next' / name for name in (
        'observable_runtime.c', 'observable_runtime.h')],
    *[workspace / 'toast-runtime-next' / name for name in (
        'toast_runtime.c', 'toast_runtime.h')],
    *[workspace / 'view-runtime-next' / name for name in (
        'view_runtime.c', 'view_runtime.h')],
    *[workspace / 'weak-runtime-next' / name for name in (
        'weak_runtime.c', 'weak_runtime.h')],
    *[workspace / 'date-runtime-next' / name for name in (
        'date_runtime.c', 'date_runtime.h',
        'simple_date_format.c', 'simple_date_format.h')],
    *[workspace / 'reentrant-lock-next' / name for name in (
        'reentrant_lock.c', 'reentrant_lock.h')],
    *[workspace / 'intent-filter-next' / name for name in (
        'intent_filter_runtime.c', 'intent_filter_runtime.h')],
    *[workspace / 'locale-runtime-next' / name for name in (
        'locale_list.c', 'locale_list.h')],
    *[workspace / 'enum-runtime-next' / name for name in (
        'enum_runtime.c', 'enum_runtime.h')],
    *[workspace / 'priority-queue-next' / name for name in (
        'priority_queue.c', 'priority_queue.h')],
    *[workspace / 'random-access-file-next' / name for name in (
        'random_access_file.c', 'random_access_file.h')],
    *[workspace / 'jce-runtime-next' / name for name in (
        'jce_runtime.c', 'jce_runtime.h')],
    *[workspace / 'system-runtime-next' / name for name in (
        'system_runtime.c', 'system_runtime.h')],
    *[workspace / 'android-os-next' / name for name in (
        'android_os.c', 'android_os.h')],
    *[workspace / 'concurrent-runtime-next' / name for name in (
        'concurrent_runtime.c', 'concurrent_runtime.h')],
    *[workspace / 'android-util-next' / name for name in (
        'android_util.c', 'android_util.h')],
]
missing_inputs = [str(path) for path in required_inputs if not path.exists()]
if missing_inputs:
    raise SystemExit('TLS candidate prerequisites missing: ' + ', '.join(missing_inputs))

out.mkdir()
shutil.copytree(base / 'source', source)
# Per-allocation identity prevents weak native map keys from aliasing a reused
# dvm_ref. Keep the preserved template immutable; compile every unit against
# this same generated runtime header.
internal = source / 'generated/dvm/dvm_internal.h'
internal_text = internal.read_text(encoding='utf-8')
assert 'uint64_t identity_generation;' not in internal_text
internal_text = internal_text.replace('struct dvm_object {',
    'struct dvm_object {\n   _Alignas(16) uint64_t identity_generation;\n'
    '   _Atomic(struct dvm_object *) gc_self;\n'
    '   uint32_t gc_flags, gc_strong_handles, gc_body_borrows, gc_payload_borrows, gc_weak_head;\n'
    '   uint8_t gc_age;', 1)
assert 'struct dvm_class {' in internal_text
internal_text = '#include "gc_runtime.h"\n' + internal_text
internal_text = internal_text.replace('struct dvm_class {',
    'struct dvm_class {\n   struct dvm_gc_layout *gc_layout;', 1)
assert 'uint32_t heap_size, heap_cap;' in internal_text
internal_text = internal_text.replace('uint32_t heap_size, heap_cap;',
    'uint32_t heap_size, heap_cap;\n   uint64_t next_object_identity;\n'
    '   struct dvm_gc_state *gc;\n   bool gc_initializing, gc_destroying, gc_execution_incomplete;', 1)
internal.write_text(internal_text, encoding='utf-8')
shutil.copy2(root/'generated/upstream_collections.inc', source/'generated/upstream_collections.inc')
shutil.copy2(workspace/'file-directory-next/file_runtime.h',source/'file_runtime.h')
ca_pem = workspace/'tls-java-next/analysis/mozilla-ca-bundle.pem'
ca_provenance = json.loads(ca_pem.with_suffix('.json').read_text(encoding='utf-8'))
ca_bytes = ca_pem.read_bytes()
if hashlib.sha256(ca_bytes).hexdigest() != ca_provenance['sha256']:
    raise SystemExit('guest CA bundle digest mismatch')
ca_blocks = __import__('re').findall(b'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', ca_bytes, __import__('re').S)
if len(ca_blocks) != ca_provenance['certificates']:
    raise SystemExit('guest CA certificate count mismatch')
ca_text = b'\n'.join(ca_blocks).decode('ascii')
(source/'mozilla_ca_bundle.h').write_text(
    '/* Mozilla CA roots from curl.se; SHA256 '+ca_provenance['sha256']+' */\n'
    'static const unsigned char guest_mozilla_ca_pem[] =\n'+
    '\n'.join(json.dumps(line+'\n') for line in ca_text.splitlines())+';\n', encoding='ascii')
for name in ('system_properties.c', 'system_properties.h'):
    shutil.copy2(workspace/'system-properties-next'/name, source/name)
for name in ('security_provider.c', 'security_provider.h'):
    shutil.copy2(workspace/'security-provider-next'/name, source/name)
for name in ('discovery_runtime.c', 'discovery_runtime.h'):
    shutil.copy2(workspace/'platform-discovery-next'/name, source/name)
for name in ('small_boxes.c', 'small_boxes.h'):
    shutil.copy2(workspace/'small-boxes-next'/name, source/name)
for name in ('battery_host.h','battery_runtime.c','battery_runtime.h','battery_runtime_test.c'):
    shutil.copy2(workspace/'battery-runtime-next'/name,source/name)
for name in ('prefs_runtime.c', 'prefs_runtime.h'):
    shutil.copy2(workspace/'preferences-runtime-next'/name, source/name)
shutil.copy2(workspace/'reflection-runtime-next/reflection_runtime.c',source/'reflection_runtime.c')
# Application-first lifecycle overlay: original MainApplication construction,
# attachBaseContext with a distinct host base Context, and original onCreate.
for name in ('android_basics.c','android_basics.h','android_basics_test.c',
             'activity_runtime.c','activity_runtime.h','view_state_runtime.c','view_state_runtime.h','window_attributes.c','window_attributes.h','display_runtime.c','display_runtime.h','surface_view_runtime.c','surface_view_runtime.h','theme_runtime.c','theme_runtime.h','layout_params_runtime.c','layout_params_runtime.h','unity_view_mount.c','unity_view_mount.h','application_lifecycle.c',
             'application_lifecycle.h'):
    shutil.copy2(workspace/'application-lifecycle-next'/name, source/name)
shutil.copy2(workspace/'application-lifecycle-next/shadow_view_declarations.inc',source/'shadow_view_declarations.inc')
shutil.copy2(root/'webview_panel.inc',source/'webview_panel.inc')
for name in ('multitouch_packet.h','multitouch_runtime.inc','multitouch_selftest.inc','multitouch_unity.inc','enqueue_drain_observer.inc','input_pipeline_observer.inc'):
    shutil.copy2(workspace/'native-elf-loader-next'/name,source/name)
for name in ('web_cookie_runtime.h','web_cookie_runtime.inc'):
    shutil.copy2(workspace/'application-lifecycle-next'/name,source/name)
shutil.copy2(workspace/'application-lifecycle-next/math_interpolator_runtime.inc',source/'math_interpolator_runtime.inc')
for name in ('task6f_download_preflight.inc','task6f_ugui_observer.inc','task6k_title_observer.inc','task8a_title_trace.inc','task8a_watch.inc'):
    shutil.copy2(workspace/'native-elf-loader-next'/name,source/name)
shutil.copy2(workspace/'sdk-runtime-spike/okio_buffer_selftest.inc',source/'okio_buffer_selftest.inc')
shutil.copy2(workspace/'sdk-runtime-spike/task_step_profile.inc',source/'task_step_profile.inc')
for name in ('okio_buffer_read.inc','okio_buffer_read_selftest.inc','okio_response_read.inc','okio_response_read_selftest.inc'):
    shutil.copy2(workspace/'sdk-runtime-spike'/name,source/name)
shutil.copy2(workspace/'sdk-runtime-spike/byte_array_input_stream_selftest.inc',source/'byte_array_input_stream_selftest.inc')
shutil.copy2(workspace/'sdk-runtime-spike/input_stream_reader_selftest.inc',source/'input_stream_reader_selftest.inc')
shutil.copy2(workspace/'sdk-runtime-spike/jni_throwable_pressure_selftest.inc',source/'jni_throwable_pressure_selftest.inc')
# Stage-B SDK initialization overlay: cache dir, StringSet preferences,
# String.trim/format, Locale, reflective constructors, ConcurrentHashMap.
for name in ('freeze180_file.inc','freeze180_resource_strings.inc','file_runtime.c','prefs_runtime.c','string_runtime.c',
             'reflection_runtime.c','platform_runtime.c','java_basics.c',
             'apk_resource_table.inc', 'unity_resource_strings.inc', 'numeric_radix_cases.inc', 'task5_split_cases.inc',
             'io_runtime.c','io_runtime.h','lrucache_runtime.c',
             'lrucache_runtime.h','latch_runtime.c','latch_runtime.h',
             'executor_extras.c','executor_extras.h','thread_extras.c',
             'executor_runtime.c','collections.c','collections_test.c',
             'thread_extras.h','thread_runtime.c','sqlite_runtime.c',
             'sqlite_runtime.h','sdk_init.c','sdk_init.h',
             'bundle_runtime.c','bundle_runtime.h',
             'content_runtime.c','content_runtime.h','settings_compat.inc',
             'network_runtime.c','network_runtime.h','notification_services.inc',
             'intent_runtime.c','intent_runtime.h','intent_launch_host.h',
             'uri_runtime.c','uri_runtime.h','java_log_runtime.c'):
    shutil.copy2(workspace/'sdk-init-next'/name, source/name)
for name in ('json_runtime.c', 'json_runtime.h', 'tokener.inc'):
    shutil.copy2(workspace/'json-runtime-next'/name, source/name)
# java_basics.c includes these .inc files; the candidate base has older copies.
for name in ('runtime_extra.inc', 'runtime_extra_test.inc'):
    shutil.copy2(workspace/'sdk-init-next'/name, source/name)
# Bounded aggregate-step guard: the base 100000 cap is below the declared
# Application.onCreate stage budget and exhausted normal OkHttp/Activity work.
shutil.copy2(workspace/'sdk-init-next/dvm.c', source/'generated/dvm/dvm.c')
for name in ('gc_runtime.h', 'gc_layout_types.h', 'gc_layout.inc', 'gc_foundation.inc', 'gc_handles.inc','gc_fast.inc','gc_stw.inc','gc_e1_matrix.inc','gc_young_selftest.inc','gil_observe.inc'):
    shutil.copy2(workspace/'sdk-init-next'/name, source/name)
shutil.copy2(workspace/'sdk-init-next/gil_perf.inc',source/'gil_perf.inc')
shutil.copy2(workspace/'sdk-init-next/gil_deep.inc',source/'gil_deep.inc')
for name in ('next_arena.c', 'next_arena.h', 'next_interpose.h',
             'thread_runtime.h', 'executor_runtime.h'):
    shutil.copy2(root/name, source/name)
# Runtime-level class contracts (String -> CharSequence).  dvm.c calls
# runtime_contract_vm_start() after every runtime installer; the ifaces block
# is allocated through the VM arena and freed once by dvm_destroy().
for name in ('runtime_contract.c', 'runtime_contract.h'):
    shutil.copy2(workspace / 'runtime-contract-next' / name, source / name)
# Collections.unmodifiableList / Arrays.asList overlay: the preserved base
# source predates the toArray implementations, so it must be copied explicitly.
for name in ('list_views.c', 'list_views.h'):
    shutil.copy2(workspace/'list-views-next'/name, source/name)
# Default Object.toString must reach the candidate; the base object_runtime.c
# only implements getClass/Math.min.
shutil.copy2(workspace/'core-object-next/object_runtime.c', source/'object_runtime.c')
for name in ('set_runtime.c', 'set_runtime.h'):
    shutil.copy2(workspace/'set-runtime-next'/name, source/name)
shutil.copy2(workspace/'asset-reader-next/asset_reader.c', source/'asset_reader.c')
# assets.inc is derived from the user's own APK and must never be committed.
# Generate it locally (manifest + optional small-asset embedding) if absent.
if not (workspace / 'asset-reader-next/assets.inc').is_file():
    apk_env = os.environ.get('BG_APK')
    candidate_apk = Path(apk_env) if apk_env else None
    if candidate_apk is None:
        found = sorted(workspace.glob('*.apk'))
        candidate_apk = found[0] if found else None
    if candidate_apk is None or not candidate_apk.is_file():
        raise SystemExit('asset-reader-next/assets.inc missing: run "python asset-reader-next/generate.py --apk <your.apk>" first (or set BG_APK)')
    subprocess.run([sys.executable, str(workspace / 'asset-reader-next/generate.py'), '--apk', str(candidate_apk)], check=True)
shutil.copy2(workspace/'asset-reader-next/assets.inc', source/'assets.inc')
if os.environ.get('UNITY_PLAYER_PROBE') == '1':
    original_apk = workspace / 'BanGDream_10.1.3.0.0.0.0_20260917_031717_9932d.apk'
    with zipfile.ZipFile(original_apk) as apk:
        member = apk.getinfo('assets/bin/Data/boot.config')
        if not 0 < member.file_size <= 65536:
            raise SystemExit(f'invalid Unity boot.config size: {member.file_size}')
        boot_config = apk.read(member)
    assets_table = source / 'assets.inc'
    assets_text = assets_table.read_text(encoding='utf-8')
    existing_row = f'{{"bin/Data/boot.config",NULL,{len(boot_config)}}},'
    if assets_text.count(existing_row) != 1:
        raise SystemExit('expected one unembedded Unity boot.config row')
    boot_bytes = ','.join(str(byte) for byte in boot_config)
    assets_text = f'static const unsigned char unity_boot_config_bytes[] = {{{boot_bytes}}};\n' + assets_text
    assets_text = assets_text.replace(existing_row,
        f'{{"bin/Data/boot.config",unity_boot_config_bytes,{len(boot_config)}}},')
    assets_table.write_text(assets_text, encoding='utf-8')
shutil.copy2(workspace/'native-jni-next/jni_forname_selftest.c',source/'jni_forname_selftest.c')
shutil.copy2(workspace/'native-jni-next/jni_activity_selftest.c',source/'jni_activity_selftest.c')
shutil.copy2(workspace/'sdk-init-next/statfs_jni_selftest.inc',source/'statfs_jni_selftest.inc')
for name in ('native_fault_probe.c','native_asset_bridge.c','native_asset_bridge.h'):
    shutil.copy2(workspace/'native-elf-loader-next'/name,source/name)
for name in ('assets.h','native_asset_backend_names.h'):
    shutil.copy2(workspace/'native-compat'/name,source/name)
shutil.copy2(workspace/'native-compat/assets.c',source/'native_assets_backend.c')
# Throwable.getStackTrace() lives in the throwable-reference overlay; the
# preserved base already matches it but future edits must be copied explicitly.
shutil.copy2(workspace/'throwable-reference-next/throwable_reference.c', source/'throwable_reference.c')
shutil.copy2(workspace/'throwable-reference-next/throwable_reference.h', source/'throwable_reference.h')
shutil.copy2(workspace/'sdk-init-next/choreographer_runtime.inc', source/'choreographer_runtime.inc')
# The candidate base carries an older package_metadata.c.  The reviewed
# implementation is the one build_integrated_login.py uses; copy it (and its
# generated metadata table) so the TLS candidate does not silently revert the
# metadata Bundle fixes.
shutil.copy2(workspace/'package-metadata-next/reviewed_metadata.c', source/'package_metadata.c')
shutil.copy2(workspace/'package-metadata-next/package_metadata.h', source/'package_metadata.h')
shutil.copy2(workspace/'package-metadata-next/metadata_values.inc', source/'metadata_values.inc')
# Original APK certificate: the DER is embedded and SHA256-checked by
# apk_signature_vm_start(); PackageInfo GET_SIGNATURES returns this certificate,
# never the HAP signing certificate.
for name in ('apk_signature_runtime.c', 'apk_signature_runtime.h'):
    shutil.copy2(workspace / 'signature-runtime-next' / name, source / name)
# R2 diagnostic: dlopen/dlsym only, never calls the original library.  It is
# compiled without next_interpose.h because it runs on the NAPI thread and
# owns libc buffers beyond the VM arena lifetime.
for name in ('native_probe.c', 'native_probe.h'):
    shutil.copy2(workspace / 'native-loader-next' / name, source / name)
for name in ('jni_abi.h', 'jni_abi_stubs.h', 'jni_util.c', 'jni_util.h',
             'jni_handles.c', 'jni_handles.h', 'jni_bridge.c', 'jni_bridge.h',
             'jni_gc_selftest.inc','gil_long_selftest.inc',
             'native_loader.c', 'native_loader.h'):
    shutil.copy2(workspace / 'native-jni-next' / name, source / name)
for name in ('elf_loader.c', 'elf_loader.h', 'elf_loader_probe.c', 'elf_loader_probe.h',
             'android_sensor_shim.c', 'android_sensor_shim.h',
             'android_looper_shim.c', 'android_looper_shim.h',
             'android_property_shim.c', 'android_property_shim.h',
             'task6m_net_probe.inc', 'android_netdb_shim.h','android_fdset_shim.c','android_fdset_shim.h','android_dlfcn.c','android_dlfcn.h','android_dlfcn_probe.c','android_dlfcn_probe.h','bionic_ctype_data.h','bionic_media_keys.inc','android_sysconf_map.inc','android_opensles_ids.inc'):
    shutil.copy2(workspace / 'native-elf-loader-next' / name, source / name)
for name in ('window_runtime.c','window_runtime.h','egl_window_runtime.c','egl_window_runtime.h'):
    shutil.copy2(workspace / 'native-window-next' / name, source / name)
for name in ('unity_constructor_probe.c','unity_constructor_probe.h','unity_jni_probe.c','unity_jni_probe.h','il2cpp_probe.c','il2cpp_probe.h','android_semaphore_shim.c','android_semaphore_shim.h','android_signal_shim.c','android_signal_shim.h','android_abi_probe.c','android_abi_probe.h','android_stdio_shim.c','android_stdio_shim.h','android_stdio_probe.c','android_stdio_probe.h','native_main_load_probe.c','native_main_load_probe.h','unity_player_probe.c','unity_player_probe.h','native_fault_probe.c','native_fault_probe.h'):
    shutil.copy2(workspace / 'native-elf-loader-next' / name, source / name)
for name in ('media_format.c','media_format.h','media_extractor.c','media_extractor.h','media_backend_probe.c','media_backend_probe.h','media_codec.c','media_codec.h','media_codec_probe.c','media_codec_probe.h'):
    shutil.copy2(workspace / 'native-media-next' / name, source / name)
for name in ('device_runtime.c', 'device_runtime.h'):
    shutil.copy2(workspace / 'device-runtime-next' / name, source / name)
for name in ('observable_runtime.c', 'observable_runtime.h'):
    shutil.copy2(workspace / 'observable-runtime-next' / name, source / name)
for name in ('toast_runtime.c', 'toast_runtime.h'):
    shutil.copy2(workspace / 'toast-runtime-next' / name, source / name)
for name in ('view_runtime.c', 'view_runtime.h'):
    shutil.copy2(workspace / 'view-runtime-next' / name, source / name)
for name in ('weak_runtime.c', 'weak_runtime.h'):
    shutil.copy2(workspace / 'weak-runtime-next' / name, source / name)
for name in ('date_runtime.c', 'date_runtime.h',
             'simple_date_format.c', 'simple_date_format.h'):
    shutil.copy2(workspace / 'date-runtime-next' / name, source / name)
for name in ('reentrant_lock.c', 'reentrant_lock.h'):
    shutil.copy2(workspace / 'reentrant-lock-next' / name, source / name)
for name in ('intent_filter_runtime.c', 'intent_filter_runtime.h'):
    shutil.copy2(workspace / 'intent-filter-next' / name, source / name)
for name in ('locale_list.c', 'locale_list.h'):
    shutil.copy2(workspace / 'locale-runtime-next' / name, source / name)
for name in ('enum_runtime.c', 'enum_runtime.h'):
    shutil.copy2(workspace / 'enum-runtime-next' / name, source / name)
for name in ('priority_queue.c', 'priority_queue.h'):
    shutil.copy2(workspace / 'priority-queue-next' / name, source / name)
for name in ('random_access_file.c', 'random_access_file.h'):
    shutil.copy2(workspace / 'random-access-file-next' / name, source / name)
for name in ('jce_runtime.c', 'jce_runtime.h'):
    shutil.copy2(workspace / 'jce-runtime-next' / name, source / name)
for name in ('system_runtime.c', 'system_runtime.h'):
    shutil.copy2(workspace / 'system-runtime-next' / name, source / name)
for name in ('android_os.c', 'android_os.h'):
    shutil.copy2(workspace / 'android-os-next' / name, source / name)
for name in ('concurrent_runtime.c', 'concurrent_runtime.h'):
    shutil.copy2(workspace / 'concurrent-runtime-next' / name, source / name)
for name in ('android_util.c', 'android_util.h'):
    shutil.copy2(workspace / 'android-util-next' / name, source / name)
# Publish the current Activity accessor on the already-staged Activity header.
# The base header predates the WeakReference bridge; keep this patch idempotent
# so a workspace copy that already declares the accessor is not duplicated.
activity_header = source / 'activity_runtime.h'
activity_header_text = activity_header.read_text(encoding='utf-8')
if 'activity_runtime_current' not in activity_header_text:
    activity_header_text = activity_header_text.replace(
        '#endif', 'dvm_ref activity_runtime_current(struct dvm *v);\n#endif', 1)
if 'activity_runtime_selftest' not in activity_header_text:
    activity_header_text = activity_header_text.replace(
        '#endif', 'bool activity_runtime_selftest(struct dvm *v, unsigned *checks);\n#endif', 1)
activity_header.write_text(activity_header_text, encoding='utf-8')
for name in ('socket_runtime.c', 'socket_runtime.h'):
    shutil.copy2(workspace / 'tls-java-next/java' / name, source / name)
for name in ('socket_shared.h', 'cert_runtime.h', 'cert_runtime.c', 'exception_hierarchy.h',
             'exception_hierarchy.c', 'ssl_runtime.h', 'ssl_runtime.c',
             'tls_loopback_selftest.h', 'tls_loopback_selftest.c',
             'tls_loopback_fixtures.h'):
    shutil.copy2(workspace / 'tls-java-next/java' / name, source / name)
for name in ('surface_runtime.c', 'surface_runtime.h'):
    shutil.copy2(workspace / 'surface-runtime-next' / name, source / name)
for name in ('native_window_jni.c', 'native_window_jni.h'):
    shutil.copy2(workspace / 'native-jni-next' / name, source / name)
for name in ('tls_client.h', 'tls_client.c', 'tls_mbedtls_user_config.h'):
    shutil.copy2(workspace / 'tls-java-next/native' / name, source / name)
for name in ('trust.c', 'trust.h', 'fixture_selftest.c', 'fixture_selftest.h', 'fixture_der.h'):
    shutil.copy2(workspace / 'tls-trust-next' / name, source / name)
for name in ('cert_objects.c', 'cert_objects.h', 'cert_selftest.c',
             'cert_selftest.h', 'expected_spki.h'):
    shutil.copy2(workspace / 'cert-objects-next' / name, source / name)
shutil.copy2(workspace / 'tls-java-next/analysis/SSLUtils.smali.txt', out / 'SSLUtils.smali.txt')
ssl_copy = source / 'ssl_runtime.c'
ssl_text = ssl_copy.read_text(encoding='utf-8')
ssl_text = ssl_text.replace('#include "../native/tls_client.h"', '#include "tls_client.h"')
ssl_text = ssl_text.replace('#include "../../tls-trust-next/trust.h"', '#include "trust.h"')
ssl_copy.write_text(ssl_text, encoding='utf-8')

strict = source / 'strict_runtime.c'
text = strict.read_text(encoding='utf-8')
task3_probe_anchor=' if(count!=3) goto cleanup;'
assert text.count(task3_probe_anchor)==1
text=text.replace(task3_probe_anchor,task3_probe_anchor+'\n extern bool reflection_task3_constructor_probe(struct dvm *);\n if(!reflection_task3_constructor_probe(v)){sdk_trace_fprintf(stderr,"TASK3_PROBE_FAIL\\n");goto cleanup;}',1)
text=text.replace(task3_probe_anchor,task3_probe_anchor+'\n extern bool string_runtime_task4_json_selftest(struct dvm *);\n if(!string_runtime_task4_json_selftest(v))goto cleanup;',1)
text=text.replace(task3_probe_anchor,task3_probe_anchor+'\n extern bool activity_runtime_task4_selftest(struct dvm *);\n if(!activity_runtime_task4_selftest(v))goto cleanup;',1)
task4_component=os.environ.get('TASK4_ACTIVITY_COMPONENT','')
if task4_component:
    assert all(c.isascii() and (c.isalnum() or c in '_/$') for c in task4_component)
    task4_player=source/'unity_player_probe.c'
    task4_text=task4_player.read_text(encoding='utf-8')
    task4_anchor=' unity_attach_debug_snapshot("frame_wait_end");'
    assert task4_text.count(task4_anchor)==1
    task4_text=task4_text.replace(task4_anchor,
        ' extern bool activity_runtime_task4_component_probe(struct dvm *,dvm_ref,const char *);\n'
        ' bool task4_tree=activity_runtime_task4_component_probe(s->vm,s->activity,'+json.dumps(task4_component)+');\n'
        ' require_record(s,fprintf(s->file,"event=task4_component_probe tree_ok=%d\\n",task4_tree));\n'+task4_anchor,1)
    task4_player.write_text(task4_text,encoding='utf-8')
    if os.environ.get('TASK5_MINIMAL_VIEW')=='1':
        task4_text=task4_text.replace(' bool task4_tree=',
            ' extern void activity_runtime_task5_minimal_layout(struct dvm *);\n'
            ' activity_runtime_task5_minimal_layout(s->vm);\n bool task4_tree=',1)
        task4_player.write_text(task4_text,encoding='utf-8')
if os.environ.get('TASK7_LOGIN_PANEL')=='1':
    shutil.copy2(workspace/'sdk-runtime-spike/login_panel_bridge.c', source/'login_panel_bridge.c')
    shutil.copy2(workspace/'sdk-runtime-spike/task6j_diagnosis.inc', source/'task6j_diagnosis.inc')
    shutil.copy2(workspace/'sdk-runtime-spike/task6l_io_selftest.inc', source/'task6l_io_selftest.inc')
    probe=source/'unity_player_probe.c'
    body=probe.read_text(encoding='utf-8')
    from app_vm_transform import player_source
    body=player_source(body)
    probe.write_text(body,encoding='utf-8')
# TASK6 explicit diagnostic; linked only for the user-requested account probe.
if os.environ.get('TASK6_LOGIN_PROBE')=='1':
    assert task4_component
    text=text.replace(task3_probe_anchor,task3_probe_anchor+'\n extern bool task6_hotpath_selftest(struct dvm *);\n if(!task6_hotpath_selftest(v)){sdk_trace_fprintf(stderr,"TASK6_HOTPATH_SELFTEST FAIL exception=%u\\n",dvm_exception(v));goto cleanup;}',1)
    shutil.copy2(workspace/'sdk-runtime-spike/task6_login_probe.c',source/'task6_login_probe.c')
    probe=source/'unity_player_probe.c'
    body=probe.read_text(encoding='utf-8')
    body=body.replace('for(int i=0;i<1200;i++)','for(int i=0;i<600;i++)',1)
    anchor=' unity_attach_debug_snapshot("frame_wait_end");'
    assert body.count(anchor)==1
    diagnostic=r''' extern bool task6_login_probe(struct dvm *);
 dvm_ref task6_lifecycle_error=dvm_exception(s->vm);bool task6_parked=s->vm->parked;
 dvm_clear_exception(s->vm);
 bool task6_ok=task6_login_probe(s->vm);
 require_record(s,fprintf(s->file,"event=task6_invoke ok=%d exception=%u\n",task6_ok,dvm_exception(s->vm)));
 dvm_ref task6_error=dvm_exception(s->vm);dvm_clear_exception(s->vm);
 for(int task6_wait=0;task6_wait<600;task6_wait++){
  unsigned task6_depth=dvm_gil_unlock_all(s->vm);struct timespec task6_pause={.tv_sec=0,.tv_nsec=100000000};nanosleep(&task6_pause,NULL);dvm_main_looper_tick(s->vm);dvm_gil_relock(s->vm,task6_depth);
 }
 require_record(s,fprintf(s->file,"event=task6_wait_finished invoke_exception=%u callback_exception=%u lifecycle_exception=%u\n",task6_error,dvm_exception(s->vm),task6_lifecycle_error));
 if(task6_lifecycle_error){s->vm->exception=task6_lifecycle_error;s->vm->parked=task6_parked;}
'''
    if os.environ.get('TASK6_SELFTEST_ONLY')=='1':
        diagnostic=' require_record(s,fprintf(s->file,"event=task6_selftest_only login_probe_called=0\\n"));\n'
    body=body.replace(anchor,diagnostic+anchor,1)
    probe.write_text(body,encoding='utf-8')
# The recovered pre-overlay template lost this function body. Restore only
# the exact known truncation, matching the archived working 72rg function.
broken_close = 'static void progress_close(void){\n__attribute__'
if broken_close in text:
    assert text.count(broken_close) == 1
    text = text.replace(broken_close,
        'static void progress_close(void){\n if(!progress_file)return;\n'
        ' fclose(progress_file);progress_file=NULL;\n}\n__attribute__', 1)
assert text.count('!file_runtime_configure(dir)||') == 2
text = text.replace('!file_runtime_configure(dir)||',
    '!file_runtime_configure(dir)||!random_access_file_configure(dir)||!android_os_configure(dir)||')
assert text.count('!strcmp(d,"Ljava/util/Collections$UnmodifiableList$Itr;")') == 1
text = text.replace('!strcmp(d,"Ljava/util/Collections$UnmodifiableList$Itr;")',
    '!strcmp(d,"Ljava/util/Collections$UnmodifiableList$Itr;")||!strcmp(d,"Ljava/util/PriorityQueue;")||!strcmp(d,"Ljava/util/PriorityQueue$Itr;")||!strcmp(d,"Ljava/util/AbstractQueue;")||!strcmp(d,"Ljava/util/Queue;")', 1)
text = text.replace('static size_t trace_used;',
    'static size_t trace_used;\n'
    'static char trace_progress_path[1024];\n'
    'static char trace_roll_path[1024];\n'
    'static void trace_roll(void){\n'
    ' if(!trace_roll_path[0]||!trace_used)return;\n'
    ' FILE *f=fopen(trace_roll_path,"a");\n'
    ' bool ok=false;\n'
    ' if(f){\n'
    '  size_t wrote=fwrite(trace,1,trace_used,f);\n'
    '  bool nl=fputc(10,f)!=EOF;\n'
    '  bool closed=fclose(f)==0;\n'
    '  ok=(wrote==trace_used)&&nl&&closed;\n'
    ' }\n'
    ' if(!ok){\n'
    '  if(trace_used+40<sizeof trace){\n'
    '   int w=snprintf(trace+trace_used,sizeof(trace)-trace_used,"\\nTRACE_ROLL_FAILED bytes=%zu\\n",trace_used);\n'
    '   if(w>0)trace_used+=(size_t)w<sizeof(trace)-trace_used?(size_t)w:sizeof(trace)-trace_used-1;\n'
    '  }\n'
    '  return;\n'
    ' }\n'
    ' size_t dropped=trace_used;trace_used=0;\n'
    ' int n=snprintf(trace,sizeof trace,"EXCEPTION TRACE_ROLLED bytes=%zu\\n",dropped);\n'
    ' if(n>0)trace_used=(size_t)n<sizeof trace?(size_t)n:sizeof(trace)-1;\n'
    '}\n'
    'static void trace_progress_write(const char *s){\n'
    ' if(!trace_progress_path[0]||!s)return;\n'
    ' static const char *const prefixes[]={"BEGIN","END ","APPLICATION_","APPLICATION ","BUDGET_STAGE","SELFTEST","DISPATCH_SELFTEST","REENTRANT","LOCALE","INTENT_FILTER","THREAD_PRIORITY","EXECUTOR","THREAD_SHUTDOWN","CLEANUP","ASYNC_FAILURE","RUNNABLE","FUTURE","MISSING_","EXCEPTION","RESULT","SDK_LOGIN","DEX_FILES","BLOCKED","JAVA_STATIC com/gsc","WEAK_CURRENT","ENGINE_LOG","ENGINE_ACTIVITY","ENGINE_THREAD","NATIVE_ASSET","EGL_WINDOW","JNI_","DISPLAY_ROTATION","DECOR_SYSTEM_UI"};\n'
    ' bool keep=false;for(size_t i=0;i<sizeof prefixes/sizeof prefixes[0];++i)if(!strncmp(s,prefixes[i],strlen(prefixes[i]))){keep=true;break;}\n'
    ' if(!keep)return;FILE *f=fopen(trace_progress_path,"a");if(!f)return;fputs(s,f);fputc(10,f);fclose(f);\n'
    '}\n', 1)
assert text.count('static char trace_progress_path[1024];') >= 1
text = text.replace('int n=vsnprintf(trace+trace_used,sizeof(trace)-trace_used,fmt,a); va_end(a);',
    'if(trace_used>114688)trace_roll();char *trace_start=trace+trace_used;int n=vsnprintf(trace_start,sizeof(trace)-trace_used,fmt,a); va_end(a);trace_progress_write(trace_start);', 1)
# A single formatted line can be larger than the remaining buffer space.
# Keep the truncated text but stamp an explicit marker into the tail so the
# rolled prefix cannot be mistaken for a complete line.
text = text.replace('if(n>0) trace_used += (size_t)n < sizeof(trace)-trace_used ? (size_t)n : sizeof(trace)-trace_used-1;',
    'if(n>0 && (size_t)n < sizeof(trace)-trace_used) trace_used += (size_t)n;\n'
    'else if(n>0){const char *mark="\\nTRACE_LINE_TRUNCATED\\n";size_t ml=strlen(mark);if(sizeof(trace)>ml+1){memcpy(trace+sizeof(trace)-ml-1,mark,ml);trace[sizeof(trace)-1]=0;}trace_used=sizeof(trace)-1;}', 1)
text = text.replace('__attribute__((visibility("default"))) const char *sdk_trace_run(const char *dir) {',
    '__attribute__((visibility("default"))) const char *sdk_trace_run(const char *dir) {\n if(dir){snprintf(trace_progress_path,sizeof trace_progress_path,"%s/login-progress.txt",dir);FILE *pf=fopen(trace_progress_path,"w");if(pf)fclose(pf);snprintf(trace_roll_path,sizeof trace_roll_path,"%s/login-trace-roll.txt",dir);FILE *rf=fopen(trace_roll_path,"w");if(rf)fclose(rf);}', 1)
text = text.replace('__attribute__((visibility("default"))) const char *sdk_login_trace_run(const char *dir) {\n pthread_mutex_lock(&runner_lock);',
    '__attribute__((visibility("default"))) const char *sdk_login_trace_run(const char *dir) {\n if(dir){snprintf(trace_progress_path,sizeof trace_progress_path,"%s/login-progress.txt",dir);FILE *pf=fopen(trace_progress_path,"w");if(pf)fclose(pf);snprintf(trace_roll_path,sizeof trace_roll_path,"%s/login-trace-roll.txt",dir);FILE *rf=fopen(trace_roll_path,"w");if(rf)fclose(rf);}\n pthread_mutex_lock(&runner_lock);', 1)
text = text.replace('sdk_trace_fprintf(stderr,"SDK_LOGIN_TRACE candidate=',
    'network_storage_probe(dir);\n sdk_trace_fprintf(stderr,"SDK_LOGIN_TRACE candidate=', 1)
network_storage_code = '''static void network_storage_probe(const char *dir){
 char path[1024]; int fd; ssize_t w,r; char buf[16];
 snprintf(path,sizeof path,"%s/net-store-probe.txt",dir?dir:"/tmp");
 fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);
 if(fd<0){sdk_trace_fprintf(stderr,"RESULT NETWORK_STORAGE files_open=fail errno=%d\\n",errno);}
 else{
  const char *msg="store-ok";w=write(fd,msg,8);fsync(fd);lseek(fd,0,SEEK_SET);memset(buf,0,sizeof buf);r=read(fd,buf,8);close(fd);unlink(path);
  sdk_trace_fprintf(stderr,"RESULT NETWORK_STORAGE files_write=%zd files_read=%zd files_data=%s\\n",w,r,(r==8&&!memcmp(buf,msg,8))?"ok":"bad");
 }
 struct addrinfo hints,*res=NULL;
 memset(&hints,0,sizeof hints);hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;
 int g=getaddrinfo("api.biligame.net","443",&hints,&res);
 if(g!=0){sdk_trace_fprintf(stderr,"RESULT NETWORK_STORAGE dns=fail gai=%d\\n",g);return;}
 int connected=0,last_errno=0;
 for(struct addrinfo *ai=res;ai;ai=ai->ai_next){
  int s=socket(ai->ai_family,ai->ai_socktype,ai->ai_protocol);
  if(s<0){last_errno=errno;continue;}
  struct timeval tv={5,0};
  setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
  setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
  if(connect(s,ai->ai_addr,ai->ai_addrlen)==0){connected=1;close(s);break;}
  last_errno=errno;close(s);
 }
 freeaddrinfo(res);
 sdk_trace_fprintf(stderr,"RESULT NETWORK_STORAGE dns=ok tcp=%s errno=%d\\n",connected?"ok":"fail",last_errno);
}
'''
text = text.replace('static bool external(', network_storage_code + 'static bool external(', 1)
text = '#include <errno.h>\n#include <netdb.h>\n#include <sys/socket.h>\n#include <sys/time.h>\n#include <netinet/in.h>\n#include <fcntl.h>\n#include <unistd.h>\n' + text
text = '#include "security_provider.h"\n#include "system_properties.h"\n#include "cert_runtime.h"\n#include "apk_signature_runtime.h"\n#include "exception_hierarchy.h"\n#include "ssl_runtime.h"\n' + text
needle = 'if(set_runtime_call('
assert text.count(needle) == 1
text = text.replace(needle,
    'if(security_provider_call(vm,c,m,s,self,a,n,out)||system_properties_call(vm,c,m,s,self,a,n,out)||ssl_runtime_call(vm,c,m,s,self,a,n,out)||cert_runtime_call(vm,c,m,s,self,a,n,out)||apk_signature_call(vm,c,m,s,self,a,n,out)||set_runtime_call(', 1)
install = 'void dvm_runtime_install(struct dvm *v) {'
assert text.count(install) == 1
text = text.replace(install,
    install + 'exception_hierarchy_vm_start(v);cert_runtime_vm_start(v);ssl_runtime_vm_start(v);security_provider_vm_start(v);surface_runtime_vm_start(v);', 1)
assert text.count('activity_runtime_vm_start(v);') == 1
text = text.replace('activity_runtime_vm_start(v);',
    'activity_runtime_vm_start(v);weak_runtime_vm_start(v);reentrant_lock_vm_start(v);locale_runtime_vm_start(v);', 1)
# A reconstructed base can compile while silently omitting the real view
# initialization needed by the dispatch selftest. Match archived 72rg order.
if 'if(!theme_runtime_vm_start(v))' not in text:
    assert text.count('locale_runtime_vm_start(v);') == 1
    view_init = (
        'if(!theme_runtime_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","theme bootstrap failed");return;}'
        'if(!layout_params_runtime_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","layout params bootstrap failed");return;}'
        'if(!view_state_runtime_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","view state bootstrap failed");return;}'
        'if(!window_attributes_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","window attributes bootstrap failed");return;}'
        'if(!display_runtime_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","display bootstrap failed");return;}'
        'if(!surface_view_runtime_vm_start(v)){dvm__throw(v,"java/lang/IllegalStateException","SurfaceView bootstrap failed");return;}')
    text = text.replace('locale_runtime_vm_start(v);', 'locale_runtime_vm_start(v);'+view_init, 1)
for init_name in ('theme_runtime', 'layout_params_runtime', 'view_state_runtime',
                  'window_attributes', 'display_runtime', 'surface_view_runtime'):
    assert text.count('if(!'+init_name+'_vm_start(v))') == 1, init_name
# The JNI bridge calls are inserted later in this file, so the VM-end hook is
# wired after those insertions; replacing here would match nothing.
assert text.count('executor_runtime_shutdown(v,100);thread_runtime_shutdown(v,100);') == 1
text = text.replace('executor_runtime_shutdown(v,100);thread_runtime_shutdown(v,100);',
    'sdk_trace_fprintf(stderr,"CLEANUP begin executor_shutdown\\n");executor_runtime_shutdown(v,100);sdk_trace_fprintf(stderr,"CLEANUP executor_shutdown_done\\n");thread_runtime_shutdown(v,100);sdk_trace_fprintf(stderr,"CLEANUP thread_shutdown_done\\n");', 1)
text = text.replace('integrated-login-v30', candidate_label)
text = '#include "discovery_runtime.h"\n'+text
text = '#include "small_boxes.h"\n'+text
text = text.replace('if(security_provider_call(', 'if(discovery_runtime_call(vm,c,m,s,self,a,n,out)||security_provider_call(', 1)
text = text.replace('security_provider_vm_start(v);', 'security_provider_vm_start(v);discovery_runtime_vm_start(v);', 1)
assert text.count('if(discovery_runtime_call(') == 1
assert text.count('discovery_runtime_vm_start(v);') == 1
text = text.replace('if(discovery_runtime_call(', 'if(small_boxes_call(vm,c,m,s,self,a,n,out)||discovery_runtime_call(', 1)
text = text.replace('discovery_runtime_vm_start(v);', 'discovery_runtime_vm_start(v);small_boxes_vm_start(v);', 1)
assert text.count('small_boxes_call(vm,c,m,s,self,a,n,out)') == 1
assert text.count('small_boxes_vm_start(v);') == 1
dispatch_test = '''bool sdk_dispatch_selftest(struct dvm *vm){
 /* Earlier diagnostic probes may call thread_runtime_shutdown(); if a worker
  * was still busy that shutdown sets the VM stop flag.  The dispatch probes
  * below run on the joined diagnostic VM and need a clean stop state. */
 vm->stopping=false;
 dvm_ref box=dvm_new_object(vm,dvm_find_class_or_external(vm,"java/lang/Byte"));
 union dvm_value arg={.i=-5},out={0};
 bool ok=box&&external(NULL,vm,"java/lang/Byte","<init>","(B)V",box,&arg,1,&out)&&!dvm_exception(vm);
 ok=ok&&external(NULL,vm,"java/lang/Byte","intValue","()I",box,NULL,0,&out)&&out.i==-5&&!dvm_exception(vm);
 struct dvm_class *weak_class=dvm_find_class_or_external(vm,"java/lang/ref/WeakReference");
 dvm_ref weak_ref=weak_class?dvm_new_object(vm,weak_class):0;
 union dvm_value referent={.l=box},weak_out={0},weak_got={0};
 bool weak=weak_ref&&external(NULL,vm,"java/lang/ref/WeakReference","<init>","(Ljava/lang/Object;)V",weak_ref,&referent,1,&weak_out)&&!dvm_exception(vm);
 weak=weak&&external(NULL,vm,"java/lang/ref/Reference","get","()Ljava/lang/Object;",weak_ref,NULL,0,&weak_got)&&weak_got.l==box&&!dvm_exception(vm);
 ok=ok&&weak;
 bool date_ok=date_runtime_selftest(vm);
 ok=ok&&date_ok;
 bool lock_ok=reentrant_lock_selftest(vm);
 bool filter_ok=intent_filter_runtime_selftest(vm);
 bool locale_ok=locale_runtime_selftest(vm);
 bool priority_ok=priority_queue_selftest(vm);
 bool raf_ok=random_access_file_selftest(vm);
 bool jce_ok=jce_runtime_selftest(vm);
 bool system_ok=system_runtime_selftest(vm);
 bool aos_ok=android_os_selftest(vm);
 bool concurrent_ok=concurrent_runtime_selftest(vm);
 unsigned activity_checks=0;
 extern bool thread_runtime_plain_selftest(struct dvm *);
 bool params_ok=layout_params_runtime_selftest(vm);
 ok=ok&&params_ok;
 bool theme_ok=theme_runtime_selftest(vm);
 ok=ok&&theme_ok;
 bool plain_ok=thread_runtime_plain_selftest(vm);
 ok=ok&&plain_ok;
 bool activity_ok=activity_runtime_selftest(vm,&activity_checks);
 bool phone_listener_ok=android_basics_phone_listener_selftest(vm,&activity_checks);
 /* The foundation VM is created with NULL hooks. Install the same real
  * dispatcher the application VM uses for this integration test, then restore
  * the caller's hooks; do not substitute a fake resource response. */
 struct dvm_hooks saved_view_hooks=vm->hooks;
 if(!vm->hooks.call_external)vm->hooks.call_external=external;
 extern bool string_runtime_dispatch_selftest(struct dvm *);
 bool string_dispatch_ok=string_runtime_dispatch_selftest(vm);
 ok=ok&&string_dispatch_ok;
 if(!string_dispatch_ok)sdk_trace_fprintf(stderr,"TASK2_STRING_DISPATCH_SELFTEST FAIL\\n");
 extern bool java_basics_hotpath_selftest(struct dvm *);
 bool hotpath_ok=java_basics_hotpath_selftest(vm);
 ok=ok&&hotpath_ok;
 if(!hotpath_ok)sdk_trace_fprintf(stderr,"TASK2_HOTPATH_SELFTEST FAIL\\n");
 bool view_state_ok=view_state_runtime_selftest(vm,android_basics_application(vm));
 view_state_ok=view_state_system_ui_selftest(vm,android_basics_application(vm))&&view_state_ok;
 view_state_ok=window_attributes_selftest(vm)&&view_state_ok;
 view_state_ok=display_runtime_selftest(vm)&&view_state_ok;
 vm->hooks=saved_view_hooks;
 sdk_trace_fprintf(stderr,"VIEW_STATE_SELFTEST %s constructed_context_identity=1 duplicate_rejected=1 resources_delegated=1 host_ui_unavailable=1\\n",view_state_ok?"PASS":"FAIL");
 bool android_util_ok=android_util_selftest(vm);
 ok=ok&&lock_ok&&filter_ok&&locale_ok&&priority_ok&&raf_ok&&jce_ok&&system_ok&&aos_ok&&concurrent_ok&&activity_ok&&phone_listener_ok&&view_state_ok&&android_util_ok;
 sdk_trace_fprintf(stderr,"DISPATCH_SELFTEST %s Byte_constructor_and_value_through_actual_external_hook=1 weak_reference=1 date_runtime=1 lock=1 intent_filter=1 locale=1 priority_queue=1 system=1 android_os=1 concurrent=1\\n",ok?"PASS":"FAIL");
 return ok;
}
'''
assert text.count('static bool get_static(') == 1
text = text.replace('static bool get_static(',dispatch_test+'static bool get_static(',1)
text = text.replace('if(java_basics_static(', 'if(weak_runtime_static(v,c,f,t,o)||device_runtime_static(v,c,f,t,o)||network_runtime_static(v,c,f,t,o)||discovery_runtime_static(v,c,f,t,o)||java_basics_static(', 1)
text = '#include "battery_runtime.h"\n'+text
assert text.count('if(small_boxes_call(') == 1
text = text.replace('if(small_boxes_call(', 'if(battery_runtime_call(vm,c,m,s,self,a,n,out)||battery_runtime_context_broadcast(vm,m,s,self,a,n,out)||small_boxes_call(',1)
assert text.count('EMPTY(dvm__ui_tick)') == 1
text = text.replace('EMPTY(dvm__ui_tick)','void dvm__ui_tick(struct dvm *vm){battery_runtime_pump(vm);}',1)
assert text.count(' certificate_batch_run();') == 1
text = text.replace(' certificate_batch_run();',' certificate_batch_run();\n if(!battery_runtime_selftest()){sdk_trace_fprintf(stderr,"RESULT BLOCKED battery fixture failed; original SDK not executed\\n");goto done;}',1)
assert text.count('collections_vm_start(v);') == 1
text = text.replace('collections_vm_start(v);',
    'collections_vm_start(v);priority_queue_vm_start(v);random_access_file_vm_start(v);jce_runtime_vm_start(v);system_runtime_vm_start(v);android_os_vm_start(v);concurrent_runtime_vm_start(v);android_util_vm_start(v);sdk_init_vm_start(v);io_runtime_vm_start(v);lrucache_runtime_vm_start(v);thread_extras_vm_start(v);', 1)
assert text.count('io_runtime_vm_start(v);') == 1
assert text.count('||throwable_reference_call(') == 1
text = text.replace('||throwable_reference_call(',
    '||io_runtime_call(vm,c,m,s,self,a,n,out)||throwable_reference_call(', 1)
assert text.count('||io_runtime_call(vm,c,m,s,self,a,n,out)||throwable_reference_call(') == 1
text = text.replace('||io_runtime_call(vm,c,m,s,self,a,n,out)||throwable_reference_call(',
    '||bundle_runtime_call(vm,c,m,s,self,a,n,out)||intent_runtime_call(vm,c,m,s,self,a,n,out)||uri_runtime_call(vm,c,m,s,self,a,n,out)||content_runtime_call(vm,c,m,s,self,a,n,out)||network_runtime_call(vm,c,m,s,self,a,n,out)||io_runtime_call(vm,c,m,s,self,a,n,out)||throwable_reference_call(', 1)
assert text.count('||executor_runtime_call(vm,c,m,s,self,a,n,out)||numeric_runtime_call(') == 1
text = text.replace('||executor_runtime_call(vm,c,m,s,self,a,n,out)||numeric_runtime_call(',
    '||latch_runtime_call(vm,c,m,s,self,a,n,out)||executor_runtime_call(vm,c,m,s,self,a,n,out)||executor_extras_call(vm,c,m,s,self,a,n,out)||numeric_runtime_call(', 1)
assert text.count('||android_basics_call(') == 1
text = text.replace('||android_basics_call(',
    '||lrucache_runtime_call(vm,c,m,s,self,a,n,out)||android_basics_call(', 1)
assert text.count('||thread_runtime_call(vm,c,m,s,self,a,n,out)||runtime_extra_call(') == 1
text = text.replace('||thread_runtime_call(vm,c,m,s,self,a,n,out)||runtime_extra_call(',
    '||weak_runtime_call(vm,c,m,s,self,a,n,out)||date_runtime_call(vm,c,m,s,self,a,n,out)||reentrant_lock_call(vm,c,m,s,self,a,n,out)||intent_filter_runtime_call(vm,c,m,s,self,a,n,out)||surface_runtime_call(vm,c,m,s,self,a,n,out)||locale_runtime_call(vm,c,m,s,self,a,n,out)||priority_queue_call(vm,c,m,s,self,a,n,out)||random_access_file_call(vm,c,m,s,self,a,n,out)||jce_runtime_call(vm,c,m,s,self,a,n,out)||system_runtime_call(vm,c,m,s,self,a,n,out)||android_os_call(vm,c,m,s,self,a,n,out)||concurrent_runtime_call(vm,c,m,s,self,a,n,out)||android_util_call(vm,c,m,s,self,a,n,out)||thread_extras_call(vm,c,m,s,self,a,n,out)||thread_runtime_call(vm,c,m,s,self,a,n,out)||view_runtime_call(vm,c,m,s,self,a,n,out)||toast_runtime_call(vm,c,m,s,self,a,n,out)||observable_runtime_call(vm,c,m,s,self,a,n,out)||runtime_extra_call(', 1)
text = ('#include "view_runtime.h"\n#include "toast_runtime.h"\n#include "observable_runtime.h"\n#include "weak_runtime.h"\n#include "date_runtime.h"\n#include "reentrant_lock.h"\n#include "intent_filter_runtime.h"\n#include "surface_runtime.h"\n#include "native_window_jni.h"\n#include "locale_list.h"\n#include "priority_queue.h"\n#include "jce_runtime.h"\n#include "system_runtime.h"\n#include "android_os.h"\n#include "concurrent_runtime.h"\n#include "android_util.h"\n#include "random_access_file.h"\n#include "device_runtime.h"\n#include "jni_bridge.h"\n#include "application_lifecycle.h"\n#include "sdk_init.h"\n'
        '#include "bundle_runtime.h"\n#include "intent_runtime.h"\n'
        '#include "uri_runtime.h"\n#include "content_runtime.h"\n#include "network_runtime.h"\n'
        '#include "io_runtime.h"\n#include "lrucache_runtime.h"\n'
        '#include "latch_runtime.h"\n#include "executor_extras.h"\n'
        '#include "thread_extras.h"\n' + text)
header_old = 'diagnostic_only=1 application_attach=not_run network=disabled'
assert text.count(header_old) == 1
text = text.replace(header_old,
    'diagnostic_only=1 application_lifecycle=original-MainApplication application_attach=pending network=disabled', 1)
budget_old = 'BUDGET 100000 instructions per stage, 256 frames, no guest native loader or network hooks'
assert text.count(budget_old) == 1
text = text.replace(budget_old,
    'BUDGET 100000 instructions per stage, Application onCreate 2000000, 256 frames, no guest native loader or network hooks', 1)
activity_old = ''' dvm_set_trace(v,0);dvm_set_step_limit(v,100000);
 const char *klass="com/bilibili/star/bili/PermissionActivity";
 struct dvm_class *cls=dvm_find_class(v,klass);
 struct dvm_method *ctor=dvm_lookup(v,klass,"<init>","()V");
 struct dvm_method *init=dvm_lookup(v,klass,"onCreate","(Landroid/os/Bundle;)V");
 if(!cls||!ctor||!init){sdk_trace_fprintf(stderr,"BLOCKED original Activity class or method lookup\\n");goto cleanup;}
 dvm_ref activity=dvm_new_object(v,cls);
 union dvm_value result={0};
 if(!activity){sdk_trace_fprintf(stderr,"BLOCKED Activity allocation\\n");goto cleanup;}
 sdk_trace_fprintf(stderr,"BEGIN original PermissionActivity constructor; original constructor creates callbacks\\n");
 bool ok=dvm_call(v,ctor,activity,NULL,0,&result);
 sdk_trace_fprintf(stderr,"END constructor returned=%d missing=%u\\n",ok,missing);
 if(!ok||missing||dvm_exception(v))goto cleanup;
 sdk_trace_fprintf(stderr,"BEGIN original onCreate with null Bundle; application attach has not run\\n");
 union dvm_value bundle={.l=0};
 ok=dvm_call(v,init,activity,&bundle,1,&result);
 sdk_trace_fprintf(stderr,"END onCreate returned=%d missing=%u\\n",ok,missing);
'''
assert text.count(activity_old) == 1
activity_new = ''' dvm_set_trace(v,0);dvm_set_step_limit(v,100000);
 sdk_trace_fprintf(stderr,"BUDGET_STAGE stage=login_vm_created total_steps=%llu stage_delta=0 call_limit=%llu aggregate_limit=%llu\\n",(unsigned long long)dvm_instructions(v),100000ull,2000000ull);
 union dvm_value result={0};
 sdk_trace_fprintf(stderr,"BEGIN_APPLICATION original MainApplication constructor; host base Context is distinct\\n");
 dvm_ref application=sdk_application_create(v);
 bool app_ready=application!=0;
 if(!app_ready)sdk_trace_fprintf(stderr,"APPLICATION_CREATE returned=0 missing=%u\\n",missing);
 if(app_ready){
  unsigned before=missing;
  bool ctor_ok=sdk_application_construct(v,application);
  sdk_trace_fprintf(stderr,"APPLICATION_CTOR returned=%d missing=%u exception=%d\\n",ctor_ok,missing-before,dvm_exception(v)!=0);
  app_ready=ctor_ok&&missing==before&&!dvm_exception(v);
 }
 if(app_ready){
  unsigned before=missing;
  bool attach_ok=sdk_application_attach(v,application);
  sdk_trace_fprintf(stderr,"APPLICATION_ATTACH returned=%d missing=%u exception=%d\\n",attach_ok,missing-before,dvm_exception(v)!=0);
  app_ready=attach_ok&&missing==before&&!dvm_exception(v);
 }
 if(app_ready){
  unsigned before=missing;
  uint64_t budget_before=dvm_instructions(v);
  dvm_set_step_limit(v,2000000);
  bool create_ok=sdk_application_on_create(v,application);
  bool create_exception=dvm_exception(v)!=0;
  bool create_complete=create_ok&&missing==before&&!create_exception;
  sdk_trace_fprintf(stderr,"APPLICATION_ONCREATE returned=%d missing=%u exception=%d ok=%d\\n",create_ok,missing-before,create_exception,create_complete);
  sdk_trace_fprintf(stderr,"BUDGET_STAGE stage=application_oncreate total_steps=%llu stage_delta=%llu call_limit=%llu aggregate_limit=%llu\\n",(unsigned long long)dvm_instructions(v),(unsigned long long)(dvm_instructions(v)-budget_before),2000000ull,2000000ull);
  app_ready=create_complete;
 }
 if(app_ready){
  /* README R5: Router state is observed read-only after the original
   * onCreate. A normal onCreate return does not prove Router is ready. */
  bool router_known=false,router_has_init=false;
  sdk_application_router_state(v,&router_known,&router_has_init);
  bool read_ok=!dvm_exception(v);
  sdk_trace_fprintf(stderr,"APPLICATION_ROUTER known=%d hasInit=%d read_ok=%d read_only=1\\n",router_known,router_has_init,read_ok);
  bool activity_allowed=read_ok&&router_known&&router_has_init;
  sdk_trace_fprintf(stderr,"ACTIVITY_GATE router_known=%d router_has_init=%d allowed=%d\\n",router_known,router_has_init,activity_allowed);
  if(!activity_allowed){
   /* README R5 negative case: the original onCreate may return normally while
    * Router.hasInit is false, Router is unknown, or the read itself throws.
    * This branch must stop before PermissionActivity without writing
    * Router.hasInit, calling Router.init, or fabricating success. */
   sdk_trace_fprintf(stderr,"APPLICATION_LIFECYCLE result=router_not_ready activity=blocked\\n");
   goto cleanup;
  }
  sdk_trace_fprintf(stderr,"APPLICATION_LIFECYCLE result=original_methods_completed attach=run\\n");
 }else{
  sdk_trace_fprintf(stderr,"APPLICATION_LIFECYCLE result=blocked attach=not_completed\\n");
  goto cleanup;
 }
 /* Activity.onCreate now reaches real business work (params2BodyMap); the old
  * 100000 per-call slice would abort it with a false boundary before the
  * aggregate 2M diagnostic guard trips.  Both stages use the same numeric
  * bound as the aggregate guard so the log cannot be misread as cumulative. */
 dvm_set_step_limit(v,2000000);
 const char *klass="com/bilibili/star/bili/PermissionActivity";
 struct dvm_class *cls=dvm_find_class(v,klass);
 struct dvm_method *ctor=dvm_lookup(v,klass,"<init>","()V");
 struct dvm_method *init=dvm_lookup(v,klass,"onCreate","(Landroid/os/Bundle;)V");
 if(!cls||!ctor||!init){sdk_trace_fprintf(stderr,"BLOCKED original Activity class or method lookup\\n");goto cleanup;}
 dvm_ref activity=dvm_new_object(v,cls);
 if(!activity){sdk_trace_fprintf(stderr,"BLOCKED Activity allocation\\n");goto cleanup;}
 sdk_trace_fprintf(stderr,"BEGIN original PermissionActivity constructor; application lifecycle completed\\n");
 uint64_t budget_before=dvm_instructions(v);
 bool ok=dvm_call(v,ctor,activity,NULL,0,&result);
 sdk_trace_fprintf(stderr,"END constructor returned=%d missing=%u\\n",ok,missing);
 sdk_trace_fprintf(stderr,"BUDGET_STAGE stage=activity_ctor total_steps=%llu stage_delta=%llu call_limit=%llu aggregate_limit=%llu\\n",(unsigned long long)dvm_instructions(v),(unsigned long long)(dvm_instructions(v)-budget_before),2000000ull,2000000ull);
 if(!ok||missing||dvm_exception(v))goto cleanup;
 sdk_trace_fprintf(stderr,"BEGIN original onCreate with null Bundle\\n");
 union dvm_value bundle={.l=0};
 budget_before=dvm_instructions(v);
 ok=dvm_call(v,init,activity,&bundle,1,&result);
 sdk_trace_fprintf(stderr,"END onCreate returned=%d missing=%u\\n",ok,missing);
 sdk_trace_fprintf(stderr,"BUDGET_STAGE stage=activity_oncreate total_steps=%llu stage_delta=%llu call_limit=%llu aggregate_limit=%llu\\n",(unsigned long long)dvm_instructions(v),(unsigned long long)(dvm_instructions(v)-budget_before),2000000ull,2000000ull);
'''
text = text.replace(activity_old, activity_new, 1)
# R3a: install the self-owned JNI bridge after each dvm_create and shut it
# down before every dvm_destroy.  The bridge is independent of the original
# libraries; failures are reported, not converted into a fake success.
assert text.count('dvm_destroy(v);') == 3
text = text.replace('dvm_destroy(v);', 'jni_bridge_shutdown(v);dvm_destroy(v);')
first_create = 'struct dvm *v=dvm_create(&h);\n if(!v){sdk_trace_fprintf(stderr,"BLOCKED VM allocation/socketpair\\n");break;}'
assert first_create in text
text = text.replace(first_create, first_create + '\n if(!jni_bridge_init(v)||!jni_bridge_install_hooks(v)){sdk_trace_fprintf(stderr,"JNI_BRIDGE init failed\\n");jni_bridge_shutdown(v);dvm_destroy(v);break;}', 1)
second_create = 'struct dvm *v=dvm_create(&h);\n if(!v){sdk_trace_fprintf(stderr,"BLOCKED VM creation\\n");goto done;}'
assert second_create in text
second_bridge = 'if(!jni_bridge_init(v)||!jni_bridge_install_hooks(v)){sdk_trace_fprintf(stderr,"JNI_BRIDGE init failed\\n");jni_bridge_shutdown(v);goto cleanup;}'
text = text.replace(second_create, second_create + '\n ' + second_bridge, 1)
# The JNI-facing Surface self-test needs an initialized bridge on this VM and
# the host window already installed by the probe.  It reports a standalone
# line; a failure is visible but does not silently skip the rest.
text = text.replace(second_bridge,
    second_bridge + 'if(!surface_jni_selftest(v)){sdk_trace_fprintf(stderr,"SURFACE_JNI_SELFTEST_AFTER_BRIDGE_FAIL\\n");goto cleanup;}', 1)
assert text.count('||set_runtime_call(') == 1
text = text.replace('||set_runtime_call(', '||layout_params_runtime_call(vm,c,m,s,self,a,n,out)||theme_runtime_call(vm,c,m,s,self,a,n,out)||surface_view_runtime_call(vm,c,m,s,self,a,n,out)||display_runtime_call(vm,c,m,s,self,a,n,out)||view_state_runtime_call(vm,c,m,s,self,a,n,out)||set_runtime_call(', 1)
if os.environ.get('TASK7_LOGIN_PANEL')=='1':
    from app_vm_transform import runtime_source
    shutil.copy2(workspace/'sdk-runtime-spike/app_vm_owner.inc',source/'app_vm_owner.inc')
    text=runtime_source(text)
text='#include "perf_attr.h"\n'+text
trace_anchor='int sdk_trace_fprintf(FILE *f, const char *fmt, ...) {'
assert text.count(trace_anchor)==1
text=text.replace(trace_anchor,trace_anchor+'\n if(!perf_trace_keep(fmt))return 0;\n uint64_t perf_trace_start __attribute__((cleanup(perf_trace_cleanup)))=perf_now();',1)
text=text.replace(trace_anchor,'static void perf_trace_cleanup(uint64_t *t){perf_trace_done(*t);}\n'+trace_anchor,1)
strict.write_text(text, encoding='utf-8')

dvm_source = source / 'generated/dvm/dvm.c'
dvm_text = dvm_source.read_text(encoding='utf-8')
dvm_text = '#include "battery_runtime.h"\n#include "small_boxes.h"\n#include "discovery_runtime.h"\n#include "security_provider.h"\n#include "ssl_runtime.h"\n#include "socket_runtime.h"\n' + dvm_text
# Teardown hook owned by android_util.c.  Declared directly instead of pulling
# android_util.h into the VM core; dvm.c already includes its own internal
# header for struct dvm.
dvm_text = 'extern void android_util_vm_end(struct dvm *vm);\nextern void android_os_vm_end(struct dvm *vm);\n' + dvm_text
destroy = 'void dvm_destroy(struct dvm *vm)\n{'
assert dvm_text.count(destroy) == 1
teardown_anchor = '   dvm_threads_finish(vm);\n'
assert dvm_text.count(teardown_anchor) == 1
# This is the single teardown entry for every VM (the battery fixture's
# temporary VM included).  Clearing android_util's cached classes here is the
# lifecycle-ownership fix: dvm_runtime_install() must never reuse a class that
# the previous VM owned.  The class_in_vm() check in android_util.c is only a
# defensive extra; a reused malloc address can make it answer yes for a stale
# pointer (72bf report: vm address reused, pair_islots=0, InternalError).
dvm_text = dvm_text.replace(teardown_anchor,
    teardown_anchor + '   android_util_vm_end(vm);\n   android_os_vm_end(vm);\n   battery_runtime_finish(vm);\n   small_boxes_vm_end(vm);\n   discovery_runtime_vm_end(vm);\n   security_provider_vm_end(vm);\n   surface_runtime_vm_end(vm);\n   ssl_runtime_cleanup();\n   socket_runtime_cleanup();\n', 1)
dvm_source.write_text(dvm_text, encoding='utf-8')

tests = source / 'java_basics_test.c'
test_text = tests.read_text(encoding='utf-8')
test_text = '#include "discovery_runtime.h"\n#include "security_provider.h"\n#include "system_properties.h"\n#include "cert_runtime.h"\n#include "exception_hierarchy.h"\n#include "ssl_runtime.h"\n#include "surface_runtime.h"\n#include "native_window_jni.h"\n' + test_text
test_text = '#include "small_boxes.h"\n'+test_text
loopback_port = os.environ.get('TLS_LOOPBACK_PORT', '').strip()
if loopback_port:
    if not loopback_port.isdigit() or not (0 < int(loopback_port) < 65536):
        raise SystemExit(f'invalid TLS_LOOPBACK_PORT: {loopback_port!r}')
    test_text = '#include "tls_loopback_selftest.h"\n' + test_text
needle = 'if(!socket_runtime_selftest(v))okay=false;'
assert test_text.count(needle) == 1
test_text = test_text.replace(needle,
    'if(!socket_runtime_selftest(v))okay=false;'
    'if(!system_properties_selftest(v))okay=false;'
    'if(!security_provider_selftest(v))okay=false;'
    'if(!discovery_runtime_selftest(v))okay=false;'
    'if(!small_boxes_selftest(v))okay=false;'
    'if(!exception_hierarchy_selftest(v))okay=false;'
    'if(!cert_runtime_selftest(v))okay=false;'
    'if(!ssl_runtime_selftest(v))okay=false;'
    'if(!surface_runtime_selftest(v))okay=false;'
    + ('if(!tls_loopback_selftest(v))okay=false;' if loopback_port else ''), 1)
test_text = test_text.replace('marker=numeric-next', 'marker='+candidate_label)
test_text = 'extern bool sdk_dispatch_selftest(struct dvm*);\n'+test_text
assert test_text.count('if(!small_boxes_selftest(v))okay=false;') == 1
test_text = test_text.replace('if(!small_boxes_selftest(v))okay=false;',
    'if(!small_boxes_selftest(v))okay=false;if(!sdk_dispatch_selftest(v))okay=false;',1)
test_text = '#include "sdk_init.h"\nextern bool java_log_runtime_selftest(struct dvm*);\n' + test_text
assert test_text.count('if(!sdk_dispatch_selftest(v))okay=false;') == 1
test_text = test_text.replace('if(!sdk_dispatch_selftest(v))okay=false;',
    'if(!sdk_dispatch_selftest(v))okay=false;if(!sdk_init_selftest(v))okay=false;',1)
assert test_text.count('if(!sdk_init_selftest(v))okay=false;') == 1
test_text = test_text.replace('if(!sdk_init_selftest(v))okay=false;',
    'if(!sdk_init_selftest(v))okay=false;if(!java_log_runtime_selftest(v))okay=false;',1)
tests.write_text(test_text, encoding='utf-8')

shutil.copy2(workspace/'sdk-init-next/native_scratch.h',source/'native_scratch.h')

common = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot='+str(sdk/'sysroot'),
          '-std=gnu11', '-fPIC', '-O1', '-I'+str(source), '-I'+str(source/'generated'),
          '-I'+str(workspace/'tls-transport-next/vendor/mbedtls-3.6.7/include'),
          '-DMBEDTLS_USER_CONFIG_FILE="tls_mbedtls_user_config.h"', '-DLUNARIA_SOCKET_COUNTERS=1', '-pthread']
for name in ('page_geometry.h','page_geometry.c','wait_context.h','wait_context.c','diagnostic_log.h','diagnostic_log.c','socket_counters.h','socket_counters.inc'):
    shutil.copy2(workspace/'native-elf-loader-next'/name,source/name)
import re
diagnostic_names=set()
for unit in [*source.rglob('*'),*(workspace/'sdk-init-next').glob('*')]:
    if unit.suffix not in ('.c','.h','.inc'):continue
    for value in re.findall(r'"([^"\n]+\.(?:txt|csv|jsonl|log))"',unit.read_text(encoding='utf-8',errors='replace')):
        name=value.rsplit('/',1)[-1]
        if '%' not in name and re.fullmatch(r'[A-Za-z0-9_.-]+',name):diagnostic_names.add(name)
diagnostic_names.difference_update({'TextAssetBundle.txt','ignoreTextAssetBundle.txt','license.txt','ext.txt','distributor.txt','web-cookie-control.txt','task6k-command.txt','task7a-command.txt','ui-correct-policy.txt','multitouch-unity-config.txt','task6m-dns-target.txt'})
(source/'diagnostic_log_names.inc').write_text(''.join(json.dumps(n)+',\n' for n in sorted(diagnostic_names)),encoding='utf-8')
common.append('-I'+str(sdk/'sysroot/usr/include/multimedia/player_framework'))
common.append('-I'+str(workspace/'sdk-init-next/vendor/build-pcre2-16'))
common.append('-I'+str(workspace/'json-runtime-next'))
common.extend(['-Werror=implicit-function-declaration','-Werror=incompatible-pointer-types',
               '-Werror=int-conversion','-Werror=conditional-type-mismatch'])
android_stdio_probe = os.environ.get('ANDROID_STDIO_PROBE') == '1'
if android_stdio_probe:
    common.append('-DLUNARIA_ANDROID_STDIO_PROBE=1')
if os.environ.get('DVM_GC_WRITE_BARRIER')=='1':
    common.append('-DDVM_GC_WRITE_BARRIER=1')
android_abi_probe = os.environ.get('ANDROID_ABI_PROBE') == '1'
if android_abi_probe:
    common.append('-DLUNARIA_ANDROID_ABI_PROBE=1')
native_media_probe = os.environ.get('NATIVE_MEDIA_PROBE') == '1'
if native_media_probe:
    common.append('-DLUNARIA_NATIVE_MEDIA_PROBE=1')
native_media_codec_probe = os.environ.get('NATIVE_MEDIA_CODEC_PROBE') == '1'
if native_media_codec_probe:
    common.append('-DLUNARIA_NATIVE_MEDIA_CODEC_PROBE=1')
unity_bootstrap = os.environ.get('UNITY_BOOTSTRAP_PROBE') == '1'
if unity_bootstrap:
    common.append('-DLUNARIA_UNITY_BOOTSTRAP=1')
nested_dlopen = os.environ.get('NESTED_DLOPEN_PROBE') == '1'
native_main_load_probe = os.environ.get('NATIVE_MAIN_LOAD_PROBE') == '1'
unity_player_probe = os.environ.get('UNITY_PLAYER_PROBE') == '1'
if unity_player_probe and native_main_load_probe:
    raise SystemExit('choose original main or player constructor diagnostic')
if unity_player_probe:
    common.append('-DLUNARIA_UNITY_PLAYER_PROBE=1')
if os.environ.get('TASK6F_DOWNLOAD_PREFLIGHT')=='1':
    if not unity_player_probe:
        raise SystemExit('TASK6f preflight requires existing UnityMain/nativeRender hook')
    common.append('-DLUNARIA_TASK6F_PREFLIGHT=1')
if os.environ.get('TASK6N_PERSISTENCE_PROBE')=='1':
    common.append('-DLUNARIA_TASK6N_PERSISTENCE_PROBE=1')
if os.environ.get('TASK6K_OBSERVER')=='1':
    if not unity_player_probe:
        raise SystemExit('TASK6k observer requires existing UnityMain/nativeRender hook')
    common.append('-DLUNARIA_TASK6K_OBSERVER=1')
if os.environ.get('TASK8A_DIAGNOSTIC')=='1':
    if os.environ.get('TASK6K_OBSERVER')!='1':
        raise SystemExit('TASK8a trace requires TASK6K_OBSERVER')
    common.append('-DLUNARIA_TASK8A=1')
if os.environ.get('TASK7_LOGIN_PANEL')=='1':
    common.append('-DLUNARIA_TASK7_LOGIN_PANEL=1')
if native_main_load_probe or unity_player_probe:
    common.append('-DLUNARIA_NATIVE_MAIN_LOAD_PROBE=1')
    if not unity_bootstrap:
        raise SystemExit('native main probe requires original main JNI bootstrap')
unity_preflight = os.environ.get('UNITY_LIBRARY_PREFLIGHT') == '1'
il2cpp_preflight = os.environ.get('IL2CPP_LIBRARY_PREFLIGHT') == '1'
if il2cpp_preflight:
    common.append('-DLUNARIA_IL2CPP_LIBRARY_PREFLIGHT=1')
unity_constructor_probe = os.environ.get('UNITY_CONSTRUCTOR_PROBE') == '1'
if il2cpp_preflight and unity_constructor_probe:
    raise SystemExit('IL2CPP inspection must not share a run with Unity initialization')
unity_jni_onload_probe = os.environ.get('UNITY_JNI_ONLOAD_PROBE') == '1'
if (native_main_load_probe or unity_player_probe) and (unity_preflight or il2cpp_preflight or unity_constructor_probe or unity_jni_onload_probe):
    raise SystemExit('original main probe must run without cached inspection/independent Unity initialization')
if unity_jni_onload_probe and not unity_constructor_probe:
    raise SystemExit('Unity JNI probe requires the dedicated constructor probe')
if unity_jni_onload_probe:
    common.append('-DLUNARIA_UNITY_JNI_ONLOAD_PROBE=1')
if unity_preflight and unity_constructor_probe:
    raise SystemExit('constructor probe and inspection preflight are mutually exclusive')
if (native_media_probe or native_media_codec_probe) and not (unity_preflight or unity_constructor_probe or native_main_load_probe or unity_player_probe):
    raise SystemExit('media probes require preflight or constructor probe')
if unity_preflight:
    common.append('-DLUNARIA_UNITY_LIBRARY_PREFLIGHT=1')
if unity_constructor_probe:
    common.append('-DLUNARIA_UNITY_CONSTRUCTOR_PROBE=1')
if unity_constructor_probe or native_main_load_probe or unity_player_probe:
    common.append('-DLUNARIA_CONSTRUCTOR_CANDIDATE='+json.dumps(candidate_label))
if unity_preflight or unity_constructor_probe or native_main_load_probe or unity_player_probe:
    unity_input=workspace/'rea-analysis-20261104/libunity.so'
    # Do not silently substitute a Unity Player from a different APK/build.
    with __import__('zipfile').ZipFile(workspace/'BanGDream_10.1.3.0.0.0.0_20260917_031717_9932d.apk') as apk:
        if hashlib.sha256(unity_input.read_bytes()).digest()!=hashlib.sha256(apk.read('lib/arm64-v8a/libunity.so')).digest():
            raise SystemExit('original libunity identity mismatch')
    shutil.copy2(unity_input,out/'libunity.so')
if il2cpp_preflight or native_main_load_probe or unity_player_probe:
    il2cpp_input=workspace/'rea-analysis-20261104/libil2cpp.so'
    with __import__('zipfile').ZipFile(workspace/'BanGDream_10.1.3.0.0.0.0_20260917_031717_9932d.apk') as apk:
        if hashlib.sha256(il2cpp_input.read_bytes()).digest()!=hashlib.sha256(apk.read('lib/arm64-v8a/libil2cpp.so')).digest():
            raise SystemExit('original libil2cpp identity mismatch')
    shutil.copy2(il2cpp_input,out/'libil2cpp.so')
if unity_player_probe:
    # Preserve the game's actual native media plugin, without relinking it.
    with zipfile.ZipFile(workspace/'BanGDream_10.1.3.0.0.0.0_20260917_031717_9932d.apk') as apk:
        cri = apk.read('lib/arm64-v8a/libcri_ware_unity.so')
    if hashlib.sha256(cri).hexdigest() != '389c4f47abcae9dbeaa9a8e5816e8a2fcf43c4555f1ab0ca7c0463c47af2f598':
        raise SystemExit('original CRI plugin identity mismatch')
    (out/'libcri_ware_unity.so').write_bytes(cri)
if nested_dlopen:
    common.append('-DLUNARIA_NESTED_DLOPEN_PROBE=1')
    for stem, unit in [('nested_leaf','leaf.c'),('nested_caller','caller.c'),('nested_skipped','leaf.c')]:
        fixture = workspace/'native-elf-loader-next/fixtures'/unit
        shutil.copy2(fixture,source/('fixture_'+stem+'.c'))
        subprocess.run(common+['-shared','-Wl,-z,max-page-size=16384','-Wl,-z,common-page-size=16384',str(fixture),'-Wl,-soname,lib'+stem+'.so',
                              '-ldl','-o',str(out/('lib'+stem+'.so'))],check=True)
if loopback_port:
    common.append('-DLUNARIA_TLS_LOOPBACK_PORT=' + loopback_port)
shutil.copy2(workspace/'sdk-init-next/gc_isolated_satb.c',source/'gc_isolated_satb.c')
for name in ('gc_isolated_satb.h', 'gc_isolated_evacuate.inc'):
    shutil.copy2(workspace/'sdk-init-next'/name, source/name)
for name in ('perf_attr.h','perf_attr.c','play_stall.inc','long_frame_breakdown.inc','frame_io_probe.h','frame_block_probe.h','ffrt_stall.inc','leak_scope.h','leak_scope.c'):
    shutil.copy2(workspace/'sdk-init-next'/name,source/name)
names = ['page_geometry.c','leak_scope.c','perf_attr.c','gc_isolated_satb.c','generated/dvm/dvm.c', 'generated/dvm/dex.c', 'strict_runtime.c',
         'java_basics.c', 'java_basics_test.c', 'android_basics.c', 'android_basics_test.c',
         'guest_profile.c', 'collections.c', 'collections_test.c', 'next_arena.c',
         'thread_runtime.c', 'thread_runtime_test.c', 'executor_runtime.c',
         'numeric_runtime.c', 'numeric_runtime_test.c', 'sqlite_runtime.c', 'activity_runtime.c', 'view_state_runtime.c', 'window_attributes.c', 'display_runtime.c', 'surface_view_runtime.c', 'theme_runtime.c', 'layout_params_runtime.c', 'unity_view_mount.c', 'egl_window_runtime.c',
         'java_log_runtime.c', 'enum_runtime.c', 'foundation_test.c', 'throwable_reference.c',
         'package_metadata.c', 'asset_reader.c', 'string_runtime.c', 'json_runtime.c',
         'file_runtime.c', 'prefs_runtime.c', 'reflection_runtime.c', 'array_runtime.c',
         'list_views.c', 'platform_runtime.c', 'object_runtime.c', 'proxy_runtime.c',
         'socket_runtime.c', 'set_runtime.c', 'exception_hierarchy.c', 'cert_runtime.c',
         'ssl_runtime.c', 'tls_client.c', 'trust.c', 'fixture_selftest.c', 'cert_objects.c',
         'tls_loopback_selftest.c', 'surface_runtime.c', 'native_window_jni.c',
         'android_sensor_shim.c', 'android_looper_shim.c', 'android_property_shim.c',
         'cert_selftest.c', 'system_properties.c', 'security_provider.c', 'discovery_runtime.c', 'small_boxes.c','battery_runtime.c','battery_runtime_test.c',
         'application_lifecycle.c','sdk_init.c','io_runtime.c',
         'lrucache_runtime.c','latch_runtime.c','executor_extras.c',
         'thread_extras.c','bundle_runtime.c','intent_runtime.c','uri_runtime.c','content_runtime.c','network_runtime.c',
         'apk_signature_runtime.c','runtime_contract.c','jni_util.c','jni_handles.c','jni_bridge.c','native_loader.c','elf_loader.c','elf_loader_probe.c','android_dlfcn.c','android_dlfcn_probe.c','window_runtime.c','device_runtime.c','observable_runtime.c','toast_runtime.c','view_runtime.c','weak_runtime.c','date_runtime.c','simple_date_format.c','reentrant_lock.c','intent_filter_runtime.c','locale_list.c','priority_queue.c','random_access_file.c','jce_runtime.c','system_runtime.c','android_os.c','concurrent_runtime.c','android_util.c']
objects = []
if os.environ.get('TASK6_LOGIN_PROBE')=='1':
    names.append('task6_login_probe.c')
if os.environ.get('TASK7_LOGIN_PANEL')=='1':
    names.append('login_panel_bridge.c')
media_native_units = ('media_format.c','media_extractor.c','media_backend_probe.c','media_codec.c','media_codec_probe.c')
names.extend(media_native_units)
names.append('android_fdset_shim.c')
names.append('unity_constructor_probe.c')
names.append('unity_jni_probe.c')
names.append('il2cpp_probe.c')
abi_native_units=('jni_forname_selftest.c', 'jni_activity_selftest.c','native_fault_probe.c','native_asset_bridge.c','native_assets_backend.c','android_semaphore_shim.c','android_signal_shim.c','android_abi_probe.c','android_stdio_shim.c','android_stdio_probe.c','native_main_load_probe.c','unity_player_probe.c')
names.extend(abi_native_units)
for name in names:
    obj = out / (Path(name).stem + '.o')
    flags = common + (['-DGAME_ASSET_PRIVATE_BACKEND=1'] if name=='native_assets_backend.c' else []) + ([] if name in ('page_geometry.c','next_arena.c', 'tls_client.c', 'elf_loader.c', 'elf_loader_probe.c', 'android_dlfcn.c','android_dlfcn_probe.c','window_runtime.c','egl_window_runtime.c','unity_constructor_probe.c','unity_jni_probe.c') + media_native_units + abi_native_units
                      else ['-include', str(source/'next_interpose.h')])
    subprocess.run(flags + ['-include',str(source/'diagnostic_log.h'),'-c', str(source/name), '-o', str(obj)], check=True)
    objects.append(obj)

for name in ('wait_context.c','diagnostic_log.c'):
    obj=out/(Path(name).stem+'.o')
    subprocess.run(common+['-c',str(source/name),'-o',str(obj)],check=True)
    objects.append(obj)

audit = []
for name, obj in zip(names, objects):
    if name in ('page_geometry.c','next_arena.c', 'tls_client.c', 'elf_loader.c', 'elf_loader_probe.c','android_dlfcn.c','android_dlfcn_probe.c','window_runtime.c','egl_window_runtime.c','unity_constructor_probe.c','unity_jni_probe.c') + media_native_units + abi_native_units:
        continue
    undefined = subprocess.check_output([str(nm), '--undefined-only', str(obj)], text=True)
    bypass = {'malloc', 'calloc', 'realloc', 'free', 'strdup', 'strndup'}.intersection(
        line.split()[-1] for line in undefined.splitlines() if line.split())
    if bypass:
        raise SystemExit(f'VM allocator bypass in {name}: {sorted(bypass)}')
    audit.append(name)

sqlite = workspace / 'sqlite-backend-next/build/libgame_sqlite.so'
if not sqlite.is_file():
    raise SystemExit(f'required SQLite dependency missing: {sqlite}')
# Re-link existing PIC objects with large-page host layout; do not change SQLite semantics.
subprocess.run(common+['-shared','-Wl,-z,max-page-size=16384','-Wl,-z,common-page-size=16384',
    '-Wl,--no-undefined',str(sqlite.parent/'sqlite3.o'),str(sqlite.parent/'game_sqlite.o'),
    '-lpthread','-ldl','-lm','-o',str(out/sqlite.name)],check=True)
library = out / 'libsdktrace.so'
subprocess.run(common + ['-shared','-Wl,-z,max-page-size=16384','-Wl,-z,common-page-size=16384', *map(str, objects), '-Wl,--no-undefined',
    str(workspace/'sdk-init-next/vendor/build-pcre2-16/libpcre2-16.a'),
    '-L'+str(out), '-L'+str(mbed_dir), '-lmbedtls', '-lmbedx509', '-lmbedcrypto',
    '-lEGL', '-lnative_vsync', '-lgame_sqlite', '-lnet_connection', '-lhilog_ndk.z', '-ldl', '-lm', '-lpthread', '-lz',
    '-lnative_media_core','-lnative_media_codecbase','-lnative_media_avsource','-lnative_media_avdemuxer','-lnative_media_acodec',
    '-o', str(library)], check=True)
manifest = {
    'unity_player_probe': unity_player_probe,
    'native_main_load_probe': native_main_load_probe,
    'android_stdio_probe': android_stdio_probe,
    'android_abi_probe': android_abi_probe,
    'il2cpp_library_preflight': il2cpp_preflight,
    'unity_jni_onload_probe': unity_jni_onload_probe,
    'unity_constructor_probe': unity_constructor_probe,
    'native_media_codec_probe': native_media_codec_probe,
    'native_media_probe': native_media_probe,
    'unity_library_preflight': unity_preflight,
    'nested_dlopen_probe': nested_dlopen,
    'unity_bootstrap_probe': unity_bootstrap,
    'candidate': candidate_label,
    'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
    'bytes': library.stat().st_size,
    'source_base': 'integrated-login-v30',
    'source_base_sha256': base_hash,
    'regex_engine': {'version':'PCRE2-10.45-16', 'sha256':hashlib.sha256(pcre_archive.read_bytes()).hexdigest()},
    'mbedtls_archives': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in required_archives},
    'allocation_audit': audit,
    'status': 'compiled only; not staged, installed, or device tested',
    'network_policy': 'external connections disabled by default; loopback only',
    'tls_loopback_selftest': {
        'enabled': bool(loopback_port),
        'port': int(loopback_port) if loopback_port else None,
        'connect_host': '127.0.0.1',
        'server_name': 'local.test',
        'trust_binder': 'guest KeyStore with generated test CA',
        'negative_case': 'unrelated CA must be rejected with javax/net/ssl/SSLException',
    },
    'limits': [
        ('device Java SSL/TLS loopback handshake exercised by TLS_LOOPBACK_SELFTEST'
         if loopback_port else
         'device Java SSL/TLS handshake and original SDK rerun remain unverified'),
        'original SDK trust path and production HTTPS remain separate checks',
        'pin enforcement and application hostname verifier path remain separate checks',
        'Mbed TLS internal allocations are outside the DVM arena',
        'client-key and non-default trust algorithms fail closed unless implemented',
    ],
}
(out / 'candidate.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print(json.dumps(manifest, indent=2))

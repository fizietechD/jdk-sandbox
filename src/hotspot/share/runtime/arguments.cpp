/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "cds/aotLogging.hpp"
#include "cds/cds_globals.hpp"
#include "cds/cdsConfig.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/javaAssertions.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/gcConfig.hpp"
#include "gc/shared/genArguments.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logConfiguration.hpp"
#include "logging/logStream.hpp"
#include "logging/logTag.hpp"
#include "memory/allocation.inline.hpp"
#include "nmt/nmtCommon.hpp"
#include "oops/compressedKlass.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/objLayout.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiAgentList.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/arguments.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/flags/jvmFlagAccess.hpp"
#include "runtime/flags/jvmFlagLimit.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/vm_version.hpp"
#include "services/management.hpp"
#include "utilities/align.hpp"
#include "utilities/checkedCast.hpp"
#include "utilities/debug.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/macros.hpp"
#include "utilities/parseInteger.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/stringUtils.hpp"
#include "utilities/systemMemoryBarrier.hpp"
#if INCLUDE_JFR
#include "jfr/jfr.hpp"
#endif

#include <limits>

static const char _default_java_launcher[] = "generic";

#define DEFAULT_JAVA_LAUNCHER _default_java_launcher

char*  Arguments::_jvm_flags_file               = nullptr;
char** Arguments::_jvm_flags_array              = nullptr;
int    Arguments::_num_jvm_flags                = 0;
char** Arguments::_jvm_args_array               = nullptr;
int    Arguments::_num_jvm_args                 = 0;
unsigned int Arguments::_addmods_count          = 0;
#if INCLUDE_JVMCI
bool   Arguments::_jvmci_module_added           = false;
#endif
char*  Arguments::_java_command                 = nullptr;
SystemProperty* Arguments::_system_properties   = nullptr;
size_t Arguments::_conservative_max_heap_alignment = 0;
Arguments::Mode Arguments::_mode                = _mixed;
const char*  Arguments::_java_vendor_url_bug    = nullptr;
const char*  Arguments::_sun_java_launcher      = DEFAULT_JAVA_LAUNCHER;
bool   Arguments::_executing_unit_tests         = false;

// These parameters are reset in method parse_vm_init_args()
bool   Arguments::_AlwaysCompileLoopMethods     = AlwaysCompileLoopMethods;
bool   Arguments::_UseOnStackReplacement        = UseOnStackReplacement;
bool   Arguments::_BackgroundCompilation        = BackgroundCompilation;
bool   Arguments::_ClipInlining                 = ClipInlining;
size_t Arguments::_default_SharedBaseAddress    = SharedBaseAddress;

bool   Arguments::_enable_preview               = false;
bool   Arguments::_has_jdwp_agent               = false;

LegacyGCLogging Arguments::_legacyGCLogging     = { nullptr, 0 };

// These are not set by the JDK's built-in launchers, but they can be set by
// programs that embed the JVM using JNI_CreateJavaVM. See comments around
// JavaVMOption in jni.h.
abort_hook_t     Arguments::_abort_hook         = nullptr;
exit_hook_t      Arguments::_exit_hook          = nullptr;
vfprintf_hook_t  Arguments::_vfprintf_hook      = nullptr;


SystemProperty *Arguments::_sun_boot_library_path = nullptr;
SystemProperty *Arguments::_java_library_path = nullptr;
SystemProperty *Arguments::_java_home = nullptr;
SystemProperty *Arguments::_java_class_path = nullptr;
SystemProperty *Arguments::_jdk_boot_class_path_append = nullptr;
SystemProperty *Arguments::_vm_info = nullptr;

GrowableArray<ModulePatchPath*> *Arguments::_patch_mod_prefix = nullptr;
PathString *Arguments::_boot_class_path = nullptr;
bool Arguments::_has_jimage = false;

char* Arguments::_ext_dirs = nullptr;

// True if -Xshare:auto option was specified.
static bool xshare_auto_cmd_line = false;

// True if -Xint/-Xmixed/-Xcomp were specified
static bool mode_flag_cmd_line = false;

struct VMInitArgsGroup {
  const JavaVMInitArgs* _args;
  JVMFlagOrigin _origin;
};

bool PathString::set_value(const char *value, AllocFailType alloc_failmode) {
  char* new_value = AllocateHeap(strlen(value)+1, mtArguments, alloc_failmode);
  if (new_value == nullptr) {
    assert(alloc_failmode == AllocFailStrategy::RETURN_NULL, "must be");
    return false;
  }
  if (_value != nullptr) {
    FreeHeap(_value);
  }
  _value = new_value;
  strcpy(_value, value);
  return true;
}

void PathString::append_value(const char *value) {
  char *sp;
  size_t len = 0;
  if (value != nullptr) {
    len = strlen(value);
    if (_value != nullptr) {
      len += strlen(_value);
    }
    sp = AllocateHeap(len+2, mtArguments);
    assert(sp != nullptr, "Unable to allocate space for new append path value");
    if (sp != nullptr) {
      if (_value != nullptr) {
        strcpy(sp, _value);
        strcat(sp, os::path_separator());
        strcat(sp, value);
        FreeHeap(_value);
      } else {
        strcpy(sp, value);
      }
      _value = sp;
    }
  }
}

PathString::PathString(const char* value) {
  if (value == nullptr) {
    _value = nullptr;
  } else {
    _value = AllocateHeap(strlen(value)+1, mtArguments);
    strcpy(_value, value);
  }
}

PathString::~PathString() {
  if (_value != nullptr) {
    FreeHeap(_value);
    _value = nullptr;
  }
}

ModulePatchPath::ModulePatchPath(const char* module_name, const char* path) {
  assert(module_name != nullptr && path != nullptr, "Invalid module name or path value");
  size_t len = strlen(module_name) + 1;
  _module_name = AllocateHeap(len, mtInternal);
  strncpy(_module_name, module_name, len); // copy the trailing null
  _path =  new PathString(path);
}

ModulePatchPath::~ModulePatchPath() {
  if (_module_name != nullptr) {
    FreeHeap(_module_name);
    _module_name = nullptr;
  }
  if (_path != nullptr) {
    delete _path;
    _path = nullptr;
  }
}

SystemProperty::SystemProperty(const char* key, const char* value, bool writeable, bool internal) : PathString(value) {
  if (key == nullptr) {
    _key = nullptr;
  } else {
    _key = AllocateHeap(strlen(key)+1, mtArguments);
    strcpy(_key, key);
  }
  _next = nullptr;
  _internal = internal;
  _writeable = writeable;
}

// Check if head of 'option' matches 'name', and sets 'tail' to the remaining
// part of the option string.
static bool match_option(const JavaVMOption *option, const char* name,
                         const char** tail) {
  size_t len = strlen(name);
  if (strncmp(option->optionString, name, len) == 0) {
    *tail = option->optionString + len;
    return true;
  } else {
    return false;
  }
}

// Check if 'option' matches 'name'. No "tail" is allowed.
static bool match_option(const JavaVMOption *option, const char* name) {
  const char* tail = nullptr;
  bool result = match_option(option, name, &tail);
  if (tail != nullptr && *tail == '\0') {
    return result;
  } else {
    return false;
  }
}

// Return true if any of the strings in null-terminated array 'names' matches.
// If tail_allowed is true, then the tail must begin with a colon; otherwise,
// the option must match exactly.
static bool match_option(const JavaVMOption* option, const char** names, const char** tail,
  bool tail_allowed) {
  for (/* empty */; *names != nullptr; ++names) {
  if (match_option(option, *names, tail)) {
      if (**tail == '\0' || (tail_allowed && **tail == ':')) {
        return true;
      }
    }
  }
  return false;
}

#if INCLUDE_JFR
static bool _has_jfr_option = false;  // is using JFR

// return true on failure
static bool match_jfr_option(const JavaVMOption** option) {
  assert((*option)->optionString != nullptr, "invariant");
  char* tail = nullptr;
  if (match_option(*option, "-XX:StartFlightRecording", (const char**)&tail)) {
    _has_jfr_option = true;
    return Jfr::on_start_flight_recording_option(option, tail);
  } else if (match_option(*option, "-XX:FlightRecorderOptions", (const char**)&tail)) {
    _has_jfr_option = true;
    return Jfr::on_flight_recorder_option(option, tail);
  }
  return false;
}

bool Arguments::has_jfr_option() {
  return _has_jfr_option;
}
#endif

static void logOption(const char* opt) {
  if (PrintVMOptions) {
    jio_fprintf(defaultStream::output_stream(), "VM option '%s'\n", opt);
  }
}

bool needs_module_property_warning = false;

#define MODULE_PROPERTY_PREFIX "jdk.module."
#define MODULE_PROPERTY_PREFIX_LEN 11
#define ADDEXPORTS "addexports"
#define ADDEXPORTS_LEN 10
#define ADDREADS "addreads"
#define ADDREADS_LEN 8
#define ADDOPENS "addopens"
#define ADDOPENS_LEN 8
#define PATCH "patch"
#define PATCH_LEN 5
#define ADDMODS "addmods"
#define ADDMODS_LEN 7
#define LIMITMODS "limitmods"
#define LIMITMODS_LEN 9
#define PATH "path"
#define PATH_LEN 4
#define UPGRADE_PATH "upgrade.path"
#define UPGRADE_PATH_LEN 12
#define ENABLE_NATIVE_ACCESS "enable.native.access"
#define ENABLE_NATIVE_ACCESS_LEN 20
#define ILLEGAL_NATIVE_ACCESS "illegal.native.access"
#define ILLEGAL_NATIVE_ACCESS_LEN 21

// Return TRUE if option matches 'property', or 'property=', or 'property.'.
static bool matches_property_suffix(const char* option, const char* property, size_t len) {
  return ((strncmp(option, property, len) == 0) &&
          (option[len] == '=' || option[len] == '.' || option[len] == '\0'));
}

// Return true if property starts with "jdk.module." and its ensuing chars match
// any of the reserved module properties.
// property should be passed without the leading "-D".
bool Arguments::is_internal_module_property(const char* property) {
  return internal_module_property_helper(property, false);
}

// Returns true if property is one of those recognized by is_internal_module_property() but
// is not supported by CDS archived full module graph.
bool Arguments::is_incompatible_cds_internal_module_property(const char* property) {
  return internal_module_property_helper(property, true);
}

bool Arguments::internal_module_property_helper(const char* property, bool check_for_cds) {
  if (strncmp(property, MODULE_PROPERTY_PREFIX, MODULE_PROPERTY_PREFIX_LEN) == 0) {
    const char* property_suffix = property + MODULE_PROPERTY_PREFIX_LEN;
    if (matches_property_suffix(property_suffix, PATCH, PATCH_LEN) ||
        matches_property_suffix(property_suffix, LIMITMODS, LIMITMODS_LEN) ||
        matches_property_suffix(property_suffix, UPGRADE_PATH, UPGRADE_PATH_LEN) ||
        matches_property_suffix(property_suffix, ILLEGAL_NATIVE_ACCESS, ILLEGAL_NATIVE_ACCESS_LEN)) {
      return true;
    }

    if (!check_for_cds) {
      // CDS notes: these properties are supported by CDS archived full module graph.
      if (matches_property_suffix(property_suffix, ADDEXPORTS, ADDEXPORTS_LEN) ||
          matches_property_suffix(property_suffix, ADDOPENS, ADDOPENS_LEN) ||
          matches_property_suffix(property_suffix, ADDREADS, ADDREADS_LEN) ||
          matches_property_suffix(property_suffix, PATH, PATH_LEN) ||
          matches_property_suffix(property_suffix, ADDMODS, ADDMODS_LEN) ||
          matches_property_suffix(property_suffix, ENABLE_NATIVE_ACCESS, ENABLE_NATIVE_ACCESS_LEN)) {
        return true;
      }
    }
  }
  return false;
}

// Process java launcher properties.
void Arguments::process_sun_java_launcher_properties(JavaVMInitArgs* args) {
  // See if sun.java.launcher is defined.
  // Must do this before setting up other system properties,
  // as some of them may depend on launcher type.
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    const char* tail;

    if (match_option(option, "-Dsun.java.launcher=", &tail)) {
      process_java_launcher_argument(tail, option->extraInfo);
      continue;
    }
    if (match_option(option, "-XX:+ExecutingUnitTests")) {
      _executing_unit_tests = true;
      continue;
    }
  }
}

// Initialize system properties key and value.
void Arguments::init_system_properties() {

  // Set up _boot_class_path which is not a property but
  // relies heavily on argument processing and the jdk.boot.class.path.append
  // property. It is used to store the underlying boot class path.
  _boot_class_path = new PathString(nullptr);

  PropertyList_add(&_system_properties, new SystemProperty("java.vm.specification.name",
                                                           "Java Virtual Machine Specification",  false));
  PropertyList_add(&_system_properties, new SystemProperty("java.vm.version", VM_Version::vm_release(),  false));
  PropertyList_add(&_system_properties, new SystemProperty("java.vm.name", VM_Version::vm_name(),  false));
  PropertyList_add(&_system_properties, new SystemProperty("jdk.debug", VM_Version::jdk_debug_level(),  false));

  // Initialize the vm.info now, but it will need updating after argument parsing.
  _vm_info = new SystemProperty("java.vm.info", VM_Version::vm_info_string(), true);

  // Following are JVMTI agent writable properties.
  // Properties values are set to nullptr and they are
  // os specific they are initialized in os::init_system_properties_values().
  _sun_boot_library_path = new SystemProperty("sun.boot.library.path", nullptr,  true);
  _java_library_path = new SystemProperty("java.library.path", nullptr,  true);
  _java_home =  new SystemProperty("java.home", nullptr,  true);
  _java_class_path = new SystemProperty("java.class.path", "",  true);
  // jdk.boot.class.path.append is a non-writeable, internal property.
  // It can only be set by either:
  //    - -Xbootclasspath/a:
  //    - AddToBootstrapClassLoaderSearch during JVMTI OnLoad phase
  _jdk_boot_class_path_append = new SystemProperty("jdk.boot.class.path.append", nullptr, false, true);

  // Add to System Property list.
  PropertyList_add(&_system_properties, _sun_boot_library_path);
  PropertyList_add(&_system_properties, _java_library_path);
  PropertyList_add(&_system_properties, _java_home);
  PropertyList_add(&_system_properties, _java_class_path);
  PropertyList_add(&_system_properties, _jdk_boot_class_path_append);
  PropertyList_add(&_system_properties, _vm_info);

  // Set OS specific system properties values
  os::init_system_properties_values();
}

// Update/Initialize System properties after JDK version number is known
void Arguments::init_version_specific_system_properties() {
  enum { bufsz = 16 };
  char buffer[bufsz];
  const char* spec_vendor = "Oracle Corporation";
  uint32_t spec_version = JDK_Version::current().major_version();

  jio_snprintf(buffer, bufsz, UINT32_FORMAT, spec_version);

  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.vendor",  spec_vendor, false));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.version", buffer, false));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.vendor", VM_Version::vm_vendor(),  false));
}

/*
 *  -XX argument processing:
 *
 *  -XX arguments are defined in several places, such as:
 *      globals.hpp, globals_<cpu>.hpp, globals_<os>.hpp, <compiler>_globals.hpp, or <gc>_globals.hpp.
 *  -XX arguments are parsed in parse_argument().
 *  -XX argument bounds checking is done in check_vm_args_consistency().
 *
 * Over time -XX arguments may change. There are mechanisms to handle common cases:
 *
 *      ALIASED: An option that is simply another name for another option. This is often
 *               part of the process of deprecating a flag, but not all aliases need
 *               to be deprecated.
 *
 *               Create an alias for an option by adding the old and new option names to the
 *               "aliased_jvm_flags" table. Delete the old variable from globals.hpp (etc).
 *
 *   DEPRECATED: An option that is supported, but a warning is printed to let the user know that
 *               support may be removed in the future. Both regular and aliased options may be
 *               deprecated.
 *
 *               Add a deprecation warning for an option (or alias) by adding an entry in the
 *               "special_jvm_flags" table and setting the "deprecated_in" field.
 *               Often an option "deprecated" in one major release will
 *               be made "obsolete" in the next. In this case the entry should also have its
 *               "obsolete_in" field set.
 *
 *     OBSOLETE: An option that has been removed (and deleted from globals.hpp), but is still accepted
 *               on the command line. A warning is printed to let the user know that option might not
 *               be accepted in the future.
 *
 *               Add an obsolete warning for an option by adding an entry in the "special_jvm_flags"
 *               table and setting the "obsolete_in" field.
 *
 *      EXPIRED: A deprecated or obsolete option that has an "accept_until" version less than or equal
 *               to the current JDK version. The system will flatly refuse to admit the existence of
 *               the flag. This allows a flag to die automatically over JDK releases.
 *
 *               Note that manual cleanup of expired options should be done at major JDK version upgrades:
 *                  - Newly expired options should be removed from the special_jvm_flags and aliased_jvm_flags tables.
 *                  - Newly obsolete or expired deprecated options should have their global variable
 *                    definitions removed (from globals.hpp, etc) and related implementations removed.
 *
 * Recommended approach for removing options:
 *
 * To remove options commonly used by customers (e.g. product -XX options), use
 * the 3-step model adding major release numbers to the deprecate, obsolete and expire columns.
 *
 * To remove internal options (e.g. diagnostic, experimental, develop options), use
 * a 2-step model adding major release numbers to the obsolete and expire columns.
 *
 * To change the name of an option, use the alias table as well as a 2-step
 * model adding major release numbers to the deprecate and expire columns.
 * Think twice about aliasing commonly used customer options.
 *
 * There are times when it is appropriate to leave a future release number as undefined.
 *
 * Tests:  Aliases should be tested in VMAliasOptions.java.
 *         Deprecated options should be tested in VMDeprecatedOptions.java.
 */

// The special_jvm_flags table declares options that are being deprecated and/or obsoleted. The
// "deprecated_in" or "obsolete_in" fields may be set to "undefined", but not both.
// When the JDK version reaches 'deprecated_in' limit, the JVM will process this flag on
// the command-line as usual, but will issue a warning.
// When the JDK version reaches 'obsolete_in' limit, the JVM will continue accepting this flag on
// the command-line, while issuing a warning and ignoring the flag value.
// Once the JDK version reaches 'expired_in' limit, the JVM will flatly refuse to admit the
// existence of the flag.
//
// MANUAL CLEANUP ON JDK VERSION UPDATES:
// This table ensures that the handling of options will update automatically when the JDK
// version is incremented, but the source code needs to be cleanup up manually:
// - As "deprecated" options age into "obsolete" or "expired" options, the associated "globals"
//   variable should be removed, as well as users of the variable.
// - As "deprecated" options age into "obsolete" options, move the entry into the
//   "Obsolete Flags" section of the table.
// - All expired options should be removed from the table.
static SpecialFlag const special_jvm_flags[] = {
  // -------------- Deprecated Flags --------------
  // --- Non-alias flags - sorted by obsolete_in then expired_in:
  { "AllowRedefinitionToAddDeleteMethods", JDK_Version::jdk(13), JDK_Version::undefined(), JDK_Version::undefined() },
  { "FlightRecorder",               JDK_Version::jdk(13), JDK_Version::undefined(), JDK_Version::undefined() },
  { "DumpSharedSpaces",             JDK_Version::jdk(18), JDK_Version::jdk(19), JDK_Version::undefined() },
  { "DynamicDumpSharedSpaces",      JDK_Version::jdk(18), JDK_Version::jdk(19), JDK_Version::undefined() },
  { "RequireSharedSpaces",          JDK_Version::jdk(18), JDK_Version::jdk(19), JDK_Version::undefined() },
  { "UseSharedSpaces",              JDK_Version::jdk(18), JDK_Version::jdk(19), JDK_Version::undefined() },
  { "LockingMode",                  JDK_Version::jdk(24), JDK_Version::jdk(26), JDK_Version::jdk(27) },
#ifdef _LP64
  { "UseCompressedClassPointers",   JDK_Version::jdk(25),  JDK_Version::jdk(26), JDK_Version::undefined() },
#endif
  { "ParallelRefProcEnabled",       JDK_Version::jdk(26),  JDK_Version::jdk(27), JDK_Version::jdk(28) },
  { "ParallelRefProcBalancingEnabled", JDK_Version::jdk(26),  JDK_Version::jdk(27), JDK_Version::jdk(28) },
  { "PSChunkLargeArrays",           JDK_Version::jdk(26),  JDK_Version::jdk(27), JDK_Version::jdk(28) },
  // --- Deprecated alias flags (see also aliased_jvm_flags) - sorted by obsolete_in then expired_in:
  { "CreateMinidumpOnCrash",        JDK_Version::jdk(9),  JDK_Version::undefined(), JDK_Version::undefined() },

  // -------------- Obsolete Flags - sorted by expired_in --------------

#ifdef LINUX
  { "UseOprofile",                  JDK_Version::jdk(25), JDK_Version::jdk(26), JDK_Version::jdk(27) },
#endif
  { "MetaspaceReclaimPolicy",       JDK_Version::undefined(), JDK_Version::jdk(21), JDK_Version::undefined() },
  { "ZGenerational",                JDK_Version::jdk(23), JDK_Version::jdk(24), JDK_Version::undefined() },
  { "ZMarkStackSpaceLimit",         JDK_Version::undefined(), JDK_Version::jdk(25), JDK_Version::undefined() },
#if defined(AARCH64)
  { "NearCpool",                    JDK_Version::undefined(), JDK_Version::jdk(25), JDK_Version::undefined() },
#endif

  { "AdaptiveSizeMajorGCDecayTimeScale",                JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "AdaptiveSizePolicyInitializingSteps",              JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "AdaptiveSizePolicyOutputInterval",                 JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "AdaptiveSizeThroughPutPolicy",                     JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "AdaptiveTimeWeight",                               JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "PausePadding",                                     JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "SurvivorPadding",                                  JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "TenuredGenerationSizeIncrement",                   JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "TenuredGenerationSizeSupplement",                  JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "TenuredGenerationSizeSupplementDecay",             JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "UseAdaptiveGenerationSizePolicyAtMajorCollection", JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "UseAdaptiveGenerationSizePolicyAtMinorCollection", JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "UseAdaptiveSizeDecayMajorGCCost",                  JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "UseAdaptiveSizePolicyFootprintGoal",               JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "UseAdaptiveSizePolicyWithSystemGC",                JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },
  { "UsePSAdaptiveSurvivorSizePolicy",                  JDK_Version::undefined(), JDK_Version::jdk(26), JDK_Version::jdk(27) },

#ifdef ASSERT
  { "DummyObsoleteTestFlag",        JDK_Version::undefined(), JDK_Version::jdk(18), JDK_Version::undefined() },
#endif

#ifdef TEST_VERIFY_SPECIAL_JVM_FLAGS
  // These entries will generate build errors.  Their purpose is to test the macros.
  { "dep > obs",                    JDK_Version::jdk(9), JDK_Version::jdk(8), JDK_Version::undefined() },
  { "dep > exp ",                   JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::jdk(8) },
  { "obs > exp ",                   JDK_Version::undefined(), JDK_Version::jdk(9), JDK_Version::jdk(8) },
  { "obs > exp",                    JDK_Version::jdk(8), JDK_Version::undefined(), JDK_Version::jdk(10) },
  { "not deprecated or obsolete",   JDK_Version::undefined(), JDK_Version::undefined(), JDK_Version::jdk(9) },
  { "dup option",                   JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::undefined() },
  { "dup option",                   JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::undefined() },
#endif

  { nullptr, JDK_Version(0), JDK_Version(0) }
};

// Flags that are aliases for other flags.
typedef struct {
  const char* alias_name;
  const char* real_name;
} AliasedFlag;

static AliasedFlag const aliased_jvm_flags[] = {
  { "CreateMinidumpOnCrash",    "CreateCoredumpOnCrash" },
  { nullptr, nullptr}
};

// Return true if "v" is less than "other", where "other" may be "undefined".
static bool version_less_than(JDK_Version v, JDK_Version other) {
  assert(!v.is_undefined(), "must be defined");
  if (!other.is_undefined() && v.compare(other) >= 0) {
    return false;
  } else {
    return true;
  }
}

static bool lookup_special_flag(const char *flag_name, SpecialFlag& flag) {
  for (size_t i = 0; special_jvm_flags[i].name != nullptr; i++) {
    if ((strcmp(special_jvm_flags[i].name, flag_name) == 0)) {
      flag = special_jvm_flags[i];
      return true;
    }
  }
  return false;
}

bool Arguments::is_obsolete_flag(const char *flag_name, JDK_Version* version) {
  assert(version != nullptr, "Must provide a version buffer");
  SpecialFlag flag;
  if (lookup_special_flag(flag_name, flag)) {
    if (!flag.obsolete_in.is_undefined()) {
      if (!version_less_than(JDK_Version::current(), flag.obsolete_in)) {
        *version = flag.obsolete_in;
        // This flag may have been marked for obsoletion in this version, but we may not
        // have actually removed it yet. Rather than ignoring it as soon as we reach
        // this version we allow some time for the removal to happen. So if the flag
        // still actually exists we process it as normal, but issue an adjusted warning.
        const JVMFlag *real_flag = JVMFlag::find_declared_flag(flag_name);
        if (real_flag != nullptr) {
          char version_str[256];
          version->to_string(version_str, sizeof(version_str));
          warning("Temporarily processing option %s; support is scheduled for removal in %s",
                  flag_name, version_str);
          return false;
        }
        return true;
      }
    }
  }
  return false;
}

int Arguments::is_deprecated_flag(const char *flag_name, JDK_Version* version) {
  assert(version != nullptr, "Must provide a version buffer");
  SpecialFlag flag;
  if (lookup_special_flag(flag_name, flag)) {
    if (!flag.deprecated_in.is_undefined()) {
      if (version_less_than(JDK_Version::current(), flag.obsolete_in) &&
          version_less_than(JDK_Version::current(), flag.expired_in)) {
        *version = flag.deprecated_in;
        return 1;
      } else {
        return -1;
      }
    }
  }
  return 0;
}

const char* Arguments::real_flag_name(const char *flag_name) {
  for (size_t i = 0; aliased_jvm_flags[i].alias_name != nullptr; i++) {
    const AliasedFlag& flag_status = aliased_jvm_flags[i];
    if (strcmp(flag_status.alias_name, flag_name) == 0) {
        return flag_status.real_name;
    }
  }
  return flag_name;
}

#ifdef ASSERT
static bool lookup_special_flag(const char *flag_name, size_t skip_index) {
  for (size_t i = 0; special_jvm_flags[i].name != nullptr; i++) {
    if ((i != skip_index) && (strcmp(special_jvm_flags[i].name, flag_name) == 0)) {
      return true;
    }
  }
  return false;
}

// Verifies the correctness of the entries in the special_jvm_flags table.
// If there is a semantic error (i.e. a bug in the table) such as the obsoletion
// version being earlier than the deprecation version, then a warning is issued
// and verification fails - by returning false. If it is detected that the table
// is out of date, with respect to the current version, then ideally a warning is
// issued but verification does not fail. This allows the VM to operate when the
// version is first updated, without needing to update all the impacted flags at
// the same time. In practice we can't issue the warning immediately when the version
// is updated as it occurs for every test and some tests are not prepared to handle
// unexpected output - see 8196739. Instead we only check if the table is up-to-date
// if the check_globals flag is true, and in addition allow a grace period and only
// check for stale flags when we hit build 25 (which is far enough into the 6 month
// release cycle that all flag updates should have been processed, whilst still
// leaving time to make the change before RDP2).
// We use a gtest to call this, passing true, so that we can detect stale flags before
// the end of the release cycle.

static const int SPECIAL_FLAG_VALIDATION_BUILD = 25;

bool Arguments::verify_special_jvm_flags(bool check_globals) {
  bool success = true;
  for (size_t i = 0; special_jvm_flags[i].name != nullptr; i++) {
    const SpecialFlag& flag = special_jvm_flags[i];
    if (lookup_special_flag(flag.name, i)) {
      warning("Duplicate special flag declaration \"%s\"", flag.name);
      success = false;
    }
    if (flag.deprecated_in.is_undefined() &&
        flag.obsolete_in.is_undefined()) {
      warning("Special flag entry \"%s\" must declare version deprecated and/or obsoleted in.", flag.name);
      success = false;
    }

    if (!flag.deprecated_in.is_undefined()) {
      if (!version_less_than(flag.deprecated_in, flag.obsolete_in)) {
        warning("Special flag entry \"%s\" must be deprecated before obsoleted.", flag.name);
        success = false;
      }

      if (!version_less_than(flag.deprecated_in, flag.expired_in)) {
        warning("Special flag entry \"%s\" must be deprecated before expired.", flag.name);
        success = false;
      }
    }

    if (!flag.obsolete_in.is_undefined()) {
      if (!version_less_than(flag.obsolete_in, flag.expired_in)) {
        warning("Special flag entry \"%s\" must be obsoleted before expired.", flag.name);
        success = false;
      }

      // if flag has become obsolete it should not have a "globals" flag defined anymore.
      if (check_globals && VM_Version::vm_build_number() >= SPECIAL_FLAG_VALIDATION_BUILD &&
          !version_less_than(JDK_Version::current(), flag.obsolete_in)) {
        if (JVMFlag::find_declared_flag(flag.name) != nullptr) {
          warning("Global variable for obsolete special flag entry \"%s\" should be removed", flag.name);
          success = false;
        }
      }

    } else if (!flag.expired_in.is_undefined()) {
      warning("Special flag entry \"%s\" must be explicitly obsoleted before expired.", flag.name);
      success = false;
    }

    if (!flag.expired_in.is_undefined()) {
      // if flag has become expired it should not have a "globals" flag defined anymore.
      if (check_globals && VM_Version::vm_build_number() >= SPECIAL_FLAG_VALIDATION_BUILD &&
          !version_less_than(JDK_Version::current(), flag.expired_in)) {
        if (JVMFlag::find_declared_flag(flag.name) != nullptr) {
          warning("Global variable for expired flag entry \"%s\" should be removed", flag.name);
          success = false;
        }
      }
    }
  }
  return success;
}
#endif

bool Arguments::atojulong(const char *s, julong* result) {
  return parse_integer(s, result);
}

Arguments::ArgsRange Arguments::check_memory_size(julong size, julong min_size, julong max_size) {
  if (size < min_size) return arg_too_small;
  if (size > max_size) return arg_too_big;
  return arg_in_range;
}

// Describe an argument out of range error
void Arguments::describe_range_error(ArgsRange errcode) {
  switch(errcode) {
  case arg_too_big:
    jio_fprintf(defaultStream::error_stream(),
                "The specified size exceeds the maximum "
                "representable size.\n");
    break;
  case arg_too_small:
  case arg_unreadable:
  case arg_in_range:
    // do nothing for now
    break;
  default:
    ShouldNotReachHere();
  }
}

static bool set_bool_flag(JVMFlag* flag, bool value, JVMFlagOrigin origin) {
  if (JVMFlagAccess::set_bool(flag, &value, origin) == JVMFlag::SUCCESS) {
    return true;
  } else {
    return false;
  }
}

static bool set_fp_numeric_flag(JVMFlag* flag, const char* value, JVMFlagOrigin origin) {
  // strtod allows leading whitespace, but our flag format does not.
  if (*value == '\0' || isspace((unsigned char) *value)) {
    return false;
  }
  char* end;
  errno = 0;
  double v = strtod(value, &end);
  if ((errno != 0) || (*end != 0)) {
    return false;
  }
  if (g_isnan(v) || !g_isfinite(v)) {
    // Currently we cannot handle these special values.
    return false;
  }

  if (JVMFlagAccess::set_double(flag, &v, origin) == JVMFlag::SUCCESS) {
    return true;
  }
  return false;
}

static bool set_numeric_flag(JVMFlag* flag, const char* value, JVMFlagOrigin origin) {
  JVMFlag::Error result = JVMFlag::WRONG_FORMAT;

  if (flag->is_int()) {
    int v;
    if (parse_integer(value, &v)) {
      result = JVMFlagAccess::set_int(flag, &v, origin);
    }
  } else if (flag->is_uint()) {
    uint v;
    if (parse_integer(value, &v)) {
      result = JVMFlagAccess::set_uint(flag, &v, origin);
    }
  } else if (flag->is_intx()) {
    intx v;
    if (parse_integer(value, &v)) {
      result = JVMFlagAccess::set_intx(flag, &v, origin);
    }
  } else if (flag->is_uintx()) {
    uintx v;
    if (parse_integer(value, &v)) {
      result = JVMFlagAccess::set_uintx(flag, &v, origin);
    }
  } else if (flag->is_uint64_t()) {
    uint64_t v;
    if (parse_integer(value, &v)) {
      result = JVMFlagAccess::set_uint64_t(flag, &v, origin);
    }
  } else if (flag->is_size_t()) {
    size_t v;
    if (parse_integer(value, &v)) {
      result = JVMFlagAccess::set_size_t(flag, &v, origin);
    }
  }

  return result == JVMFlag::SUCCESS;
}

static bool set_string_flag(JVMFlag* flag, const char* value, JVMFlagOrigin origin) {
  if (value[0] == '\0') {
    value = nullptr;
  }
  if (JVMFlagAccess::set_ccstr(flag, &value, origin) != JVMFlag::SUCCESS) return false;
  // Contract:  JVMFlag always returns a pointer that needs freeing.
  FREE_C_HEAP_ARRAY(char, value);
  return true;
}

static bool append_to_string_flag(JVMFlag* flag, const char* new_value, JVMFlagOrigin origin) {
  const char* old_value = "";
  if (JVMFlagAccess::get_ccstr(flag, &old_value) != JVMFlag::SUCCESS) return false;
  size_t old_len = old_value != nullptr ? strlen(old_value) : 0;
  size_t new_len = strlen(new_value);
  const char* value;
  char* free_this_too = nullptr;
  if (old_len == 0) {
    value = new_value;
  } else if (new_len == 0) {
    value = old_value;
  } else {
     size_t length = old_len + 1 + new_len + 1;
     char* buf = NEW_C_HEAP_ARRAY(char, length, mtArguments);
    // each new setting adds another LINE to the switch:
    jio_snprintf(buf, length, "%s\n%s", old_value, new_value);
    value = buf;
    free_this_too = buf;
  }
  (void) JVMFlagAccess::set_ccstr(flag, &value, origin);
  // JVMFlag always returns a pointer that needs freeing.
  FREE_C_HEAP_ARRAY(char, value);
  // JVMFlag made its own copy, so I must delete my own temp. buffer.
  FREE_C_HEAP_ARRAY(char, free_this_too);
  return true;
}

const char* Arguments::handle_aliases_and_deprecation(const char* arg) {
  const char* real_name = real_flag_name(arg);
  JDK_Version since = JDK_Version();
  switch (is_deprecated_flag(arg, &since)) {
  case -1: {
      // Obsolete or expired, so don't process normally,
      // but allow for an obsolete flag we're still
      // temporarily allowing.
      if (!is_obsolete_flag(arg, &since)) {
        return real_name;
      }
      // Note if we're not considered obsolete then we can't be expired either
      // as obsoletion must come first.
      return nullptr;
    }
    case 0:
      return real_name;
    case 1: {
      char version[256];
      since.to_string(version, sizeof(version));
      if (real_name != arg) {
        warning("Option %s was deprecated in version %s and will likely be removed in a future release. Use option %s instead.",
                arg, version, real_name);
      } else {
        warning("Option %s was deprecated in version %s and will likely be removed in a future release.",
                arg, version);
      }
      return real_name;
    }
  }
  ShouldNotReachHere();
  return nullptr;
}

#define BUFLEN 255

JVMFlag* Arguments::find_jvm_flag(const char* name, size_t name_length) {
  char name_copied[BUFLEN+1];
  if (name[name_length] != 0) {
    if (name_length > BUFLEN) {
      return nullptr;
    } else {
      strncpy(name_copied, name, name_length);
      name_copied[name_length] = '\0';
      name = name_copied;
    }
  }

  const char* real_name = Arguments::handle_aliases_and_deprecation(name);
  if (real_name == nullptr) {
    return nullptr;
  }
  JVMFlag* flag = JVMFlag::find_flag(real_name);
  return flag;
}

bool Arguments::parse_argument(const char* arg, JVMFlagOrigin origin) {
  bool is_bool = false;
  bool bool_val = false;
  char c = *arg;
  if (c == '+' || c == '-') {
    is_bool = true;
    bool_val = (c == '+');
    arg++;
  }

  const char* name = arg;
  while (true) {
    c = *arg;
    if (isalnum(c) || (c == '_')) {
      ++arg;
    } else {
      break;
    }
  }

  size_t name_len = size_t(arg - name);
  if (name_len == 0) {
    return false;
  }

  JVMFlag* flag = find_jvm_flag(name, name_len);
  if (flag == nullptr) {
    return false;
  }

  if (is_bool) {
    if (*arg != 0) {
      // Error -- extra characters such as -XX:+BoolFlag=123
      return false;
    }
    return set_bool_flag(flag, bool_val, origin);
  }

  if (arg[0] == '=') {
    const char* value = arg + 1;
    if (flag->is_ccstr()) {
      if (flag->ccstr_accumulates()) {
        return append_to_string_flag(flag, value, origin);
      } else {
        return set_string_flag(flag, value, origin);
      }
    } else if (flag->is_double()) {
      return set_fp_numeric_flag(flag, value, origin);
    } else {
      return set_numeric_flag(flag, value, origin);
    }
  }

  if (arg[0] == ':' && arg[1] == '=') {
    // -XX:Foo:=xxx will reset the string flag to the given value.
    const char* value = arg + 2;
    return set_string_flag(flag, value, origin);
  }

  return false;
}

void Arguments::add_string(char*** bldarray, int* count, const char* arg) {
  assert(bldarray != nullptr, "illegal argument");

  if (arg == nullptr) {
    return;
  }

  int new_count = *count + 1;

  // expand the array and add arg to the last element
  if (*bldarray == nullptr) {
    *bldarray = NEW_C_HEAP_ARRAY(char*, new_count, mtArguments);
  } else {
    *bldarray = REALLOC_C_HEAP_ARRAY(char*, *bldarray, new_count, mtArguments);
  }
  (*bldarray)[*count] = os::strdup_check_oom(arg);
  *count = new_count;
}

void Arguments::build_jvm_args(const char* arg) {
  add_string(&_jvm_args_array, &_num_jvm_args, arg);
}

void Arguments::build_jvm_flags(const char* arg) {
  add_string(&_jvm_flags_array, &_num_jvm_flags, arg);
}

// utility function to return a string that concatenates all
// strings in a given char** array
const char* Arguments::build_resource_string(char** args, int count) {
  if (args == nullptr || count == 0) {
    return nullptr;
  }
  size_t length = 0;
  for (int i = 0; i < count; i++) {
    length += strlen(args[i]) + 1; // add 1 for a space or null terminating character
  }
  char* s = NEW_RESOURCE_ARRAY(char, length);
  char* dst = s;
  for (int j = 0; j < count; j++) {
    size_t offset = strlen(args[j]) + 1; // add 1 for a space or null terminating character
    jio_snprintf(dst, length, "%s ", args[j]); // jio_snprintf will replace the last space character with null character
    dst += offset;
    length -= offset;
  }
  return (const char*) s;
}

void Arguments::print_on(outputStream* st) {
  st->print_cr("VM Arguments:");
  if (num_jvm_flags() > 0) {
    st->print("jvm_flags: "); print_jvm_flags_on(st);
    st->cr();
  }
  if (num_jvm_args() > 0) {
    st->print("jvm_args: "); print_jvm_args_on(st);
    st->cr();
  }
  st->print_cr("java_command: %s", java_command() ? java_command() : "<unknown>");
  if (_java_class_path != nullptr) {
    char* path = _java_class_path->value();
    size_t len = strlen(path);
    st->print("java_class_path (initial): ");
    // Avoid using st->print_cr() because path length maybe longer than O_BUFLEN.
    if (len == 0) {
      st->print_raw_cr("<not set>");
    } else {
      st->print_raw_cr(path, len);
    }
  }
  st->print_cr("Launcher Type: %s", _sun_java_launcher);
}

void Arguments::print_summary_on(outputStream* st) {
  // Print the command line.  Environment variables that are helpful for
  // reproducing the problem are written later in the hs_err file.
  // flags are from setting file
  if (num_jvm_flags() > 0) {
    st->print_raw("Settings File: ");
    print_jvm_flags_on(st);
    st->cr();
  }
  // args are the command line and environment variable arguments.
  st->print_raw("Command Line: ");
  if (num_jvm_args() > 0) {
    print_jvm_args_on(st);
  }
  // this is the classfile and any arguments to the java program
  if (java_command() != nullptr) {
    st->print("%s", java_command());
  }
  st->cr();
}

void Arguments::print_jvm_flags_on(outputStream* st) {
  if (_num_jvm_flags > 0) {
    for (int i=0; i < _num_jvm_flags; i++) {
      st->print("%s ", _jvm_flags_array[i]);
    }
  }
}

void Arguments::print_jvm_args_on(outputStream* st) {
  if (_num_jvm_args > 0) {
    for (int i=0; i < _num_jvm_args; i++) {
      st->print("%s ", _jvm_args_array[i]);
    }
  }
}

bool Arguments::process_argument(const char* arg,
                                 jboolean ignore_unrecognized,
                                 JVMFlagOrigin origin) {
  JDK_Version since = JDK_Version();

  if (parse_argument(arg, origin)) {
    return true;
  }

  // Determine if the flag has '+', '-', or '=' characters.
  bool has_plus_minus = (*arg == '+' || *arg == '-');
  const char* const argname = has_plus_minus ? arg + 1 : arg;

  size_t arg_len;
  const char* equal_sign = strchr(argname, '=');
  if (equal_sign == nullptr) {
    arg_len = strlen(argname);
  } else {
    arg_len = equal_sign - argname;
  }

  // Only make the obsolete check for valid arguments.
  if (arg_len <= BUFLEN) {
    // Construct a string which consists only of the argument name without '+', '-', or '='.
    char stripped_argname[BUFLEN+1]; // +1 for '\0'
    jio_snprintf(stripped_argname, arg_len+1, "%s", argname); // +1 for '\0'
    if (is_obsolete_flag(stripped_argname, &since)) {
      char version[256];
      since.to_string(version, sizeof(version));
      warning("Ignoring option %s; support was removed in %s", stripped_argname, version);
      return true;
    }
  }

  // For locked flags, report a custom error message if available.
  // Otherwise, report the standard unrecognized VM option.
  const JVMFlag* found_flag = JVMFlag::find_declared_flag((const char*)argname, arg_len);
  if (found_flag != nullptr) {
    char locked_message_buf[BUFLEN];
    JVMFlag::MsgType msg_type = found_flag->get_locked_message(locked_message_buf, BUFLEN);
    if (strlen(locked_message_buf) != 0) {
#ifdef PRODUCT
      bool mismatched = msg_type == JVMFlag::DEVELOPER_FLAG_BUT_PRODUCT_BUILD;
      if (ignore_unrecognized && mismatched) {
        return true;
      }
#endif
      jio_fprintf(defaultStream::error_stream(), "%s", locked_message_buf);
    }
    if (found_flag->is_bool() && !has_plus_minus) {
      jio_fprintf(defaultStream::error_stream(),
        "Missing +/- setting for VM option '%s'\n", argname);
    } else if (!found_flag->is_bool() && has_plus_minus) {
      jio_fprintf(defaultStream::error_stream(),
        "Unexpected +/- setting in VM option '%s'\n", argname);
    } else {
      jio_fprintf(defaultStream::error_stream(),
        "Improperly specified VM option '%s'\n", argname);
    }
  } else {
    if (ignore_unrecognized) {
      return true;
    }
    jio_fprintf(defaultStream::error_stream(),
                "Unrecognized VM option '%s'\n", argname);
    JVMFlag* fuzzy_matched = JVMFlag::fuzzy_match((const char*)argname, arg_len, true);
    if (fuzzy_matched != nullptr) {
      jio_fprintf(defaultStream::error_stream(),
                  "Did you mean '%s%s%s'?\n",
                  (fuzzy_matched->is_bool()) ? "(+/-)" : "",
                  fuzzy_matched->name(),
                  (fuzzy_matched->is_bool()) ? "" : "=<value>");
    }
  }

  // allow for commandline "commenting out" options like -XX:#+Verbose
  return arg[0] == '#';
}

bool Arguments::process_settings_file(const char* file_name, bool should_exist, jboolean ignore_unrecognized) {
  FILE* stream = os::fopen(file_name, "rb");
  if (stream == nullptr) {
    if (should_exist) {
      jio_fprintf(defaultStream::error_stream(),
                  "Could not open settings file %s\n", file_name);
      return false;
    } else {
      return true;
    }
  }

  char token[1024];
  int  pos = 0;

  bool in_white_space = true;
  bool in_comment     = false;
  bool in_quote       = false;
  int  quote_c        = 0;
  bool result         = true;

  int c = getc(stream);
  while(c != EOF && pos < (int)(sizeof(token)-1)) {
    if (in_white_space) {
      if (in_comment) {
        if (c == '\n') in_comment = false;
      } else {
        if (c == '#') in_comment = true;
        else if (!isspace((unsigned char) c)) {
          in_white_space = false;
          token[pos++] = checked_cast<char>(c);
        }
      }
    } else {
      if (c == '\n' || (!in_quote && isspace((unsigned char) c))) {
        // token ends at newline, or at unquoted whitespace
        // this allows a way to include spaces in string-valued options
        token[pos] = '\0';
        logOption(token);
        result &= process_argument(token, ignore_unrecognized, JVMFlagOrigin::CONFIG_FILE);
        build_jvm_flags(token);
        pos = 0;
        in_white_space = true;
        in_quote = false;
      } else if (!in_quote && (c == '\'' || c == '"')) {
        in_quote = true;
        quote_c = c;
      } else if (in_quote && (c == quote_c)) {
        in_quote = false;
      } else {
        token[pos++] = checked_cast<char>(c);
      }
    }
    c = getc(stream);
  }
  if (pos > 0) {
    token[pos] = '\0';
    result &= process_argument(token, ignore_unrecognized, JVMFlagOrigin::CONFIG_FILE);
    build_jvm_flags(token);
  }
  fclose(stream);
  return result;
}

//=============================================================================================================
// Parsing of properties (-D)

const char* Arguments::get_property(const char* key) {
  return PropertyList_get_value(system_properties(), key);
}

bool Arguments::add_property(const char* prop, PropertyWriteable writeable, PropertyInternal internal) {
  const char* eq = strchr(prop, '=');
  const char* key;
  const char* value = "";

  if (eq == nullptr) {
    // property doesn't have a value, thus use passed string
    key = prop;
  } else {
    // property have a value, thus extract it and save to the
    // allocated string
    size_t key_len = eq - prop;
    char* tmp_key = AllocateHeap(key_len + 1, mtArguments);

    jio_snprintf(tmp_key, key_len + 1, "%s", prop);
    key = tmp_key;

    value = &prop[key_len + 1];
  }

  if (internal == ExternalProperty) {
    CDSConfig::check_incompatible_property(key, value);
  }

  if (strcmp(key, "java.compiler") == 0) {
    // we no longer support java.compiler system property, log a warning and let it get
    // passed to Java, like any other system property
    if (strlen(value) == 0 || strcasecmp(value, "NONE") == 0) {
        // for applications using NONE or empty value, log a more informative message
        warning("The java.compiler system property is obsolete and no longer supported, use -Xint");
    } else {
        warning("The java.compiler system property is obsolete and no longer supported.");
    }
  } else if (strcmp(key, "sun.boot.library.path") == 0) {
    // append is true, writable is true, internal is false
    PropertyList_unique_add(&_system_properties, key, value, AppendProperty,
                            WriteableProperty, ExternalProperty);
  } else {
    if (strcmp(key, "sun.java.command") == 0) {
      char *old_java_command = _java_command;
      _java_command = os::strdup_check_oom(value, mtArguments);
      if (old_java_command != nullptr) {
        os::free(old_java_command);
      }
    } else if (strcmp(key, "java.vendor.url.bug") == 0) {
      // If this property is set on the command line then its value will be
      // displayed in VM error logs as the URL at which to submit such logs.
      // Normally the URL displayed in error logs is different from the value
      // of this system property, so a different property should have been
      // used here, but we leave this as-is in case someone depends upon it.
      const char* old_java_vendor_url_bug = _java_vendor_url_bug;
      // save it in _java_vendor_url_bug, so JVM fatal error handler can access
      // its value without going through the property list or making a Java call.
      _java_vendor_url_bug = os::strdup_check_oom(value, mtArguments);
      if (old_java_vendor_url_bug != nullptr) {
        os::free((void *)old_java_vendor_url_bug);
      }
    }

    // Create new property and add at the end of the list
    PropertyList_unique_add(&_system_properties, key, value, AddProperty, writeable, internal);
  }

  if (key != prop) {
    // SystemProperty copy passed value, thus free previously allocated
    // memory
    FreeHeap((void *)key);
  }

  return true;
}

//===========================================================================================================
// Setting int/mixed/comp mode flags

void Arguments::set_mode_flags(Mode mode) {
  // Set up default values for all flags.
  // If you add a flag to any of the branches below,
  // add a default value for it here.
  _mode                      = mode;

  // Ensure Agent_OnLoad has the correct initial values.
  // This may not be the final mode; mode may change later in onload phase.
  PropertyList_unique_add(&_system_properties, "java.vm.info",
                          VM_Version::vm_info_string(), AddProperty, UnwriteableProperty, ExternalProperty);

  UseInterpreter             = true;
  UseCompiler                = true;
  UseLoopCounter             = true;

  // Default values may be platform/compiler dependent -
  // use the saved values
  ClipInlining               = Arguments::_ClipInlining;
  AlwaysCompileLoopMethods   = Arguments::_AlwaysCompileLoopMethods;
  UseOnStackReplacement      = Arguments::_UseOnStackReplacement;
  BackgroundCompilation      = Arguments::_BackgroundCompilation;

  // Change from defaults based on mode
  switch (mode) {
  default:
    ShouldNotReachHere();
    break;
  case _int:
    UseCompiler              = false;
    UseLoopCounter           = false;
    AlwaysCompileLoopMethods = false;
    UseOnStackReplacement    = false;
    break;
  case _mixed:
    // same as default
    break;
  case _comp:
    UseInterpreter           = false;
    BackgroundCompilation    = false;
    ClipInlining             = false;
    break;
  }
}

// Conflict: required to use shared spaces (-Xshare:on), but
// incompatible command line options were chosen.
void Arguments::no_shared_spaces(const char* message) {
  if (RequireSharedSpaces) {
    aot_log_error(aot)("%s is incompatible with other specified options.",
                   CDSConfig::new_aot_flags_used() ? "AOT cache" : "CDS");
    if (CDSConfig::new_aot_flags_used()) {
      vm_exit_during_initialization("Unable to use AOT cache", message);
    } else {
      vm_exit_during_initialization("Unable to use shared archive", message);
    }
  } else {
    if (CDSConfig::new_aot_flags_used()) {
      log_warning(aot)("Unable to use AOT cache: %s", message);
    } else {
      aot_log_info(aot)("Unable to use shared archive: %s", message);
    }
    UseSharedSpaces = false;
  }
}

static void set_object_alignment() {
  // Object alignment.
  assert(is_power_of_2(ObjectAlignmentInBytes), "ObjectAlignmentInBytes must be power of 2");
  MinObjAlignmentInBytes     = ObjectAlignmentInBytes;
  assert(MinObjAlignmentInBytes >= HeapWordsPerLong * HeapWordSize, "ObjectAlignmentInBytes value is too small");
  MinObjAlignment            = MinObjAlignmentInBytes / HeapWordSize;
  assert(MinObjAlignmentInBytes == MinObjAlignment * HeapWordSize, "ObjectAlignmentInBytes value is incorrect");
  MinObjAlignmentInBytesMask = MinObjAlignmentInBytes - 1;

  LogMinObjAlignmentInBytes  = exact_log2(ObjectAlignmentInBytes);
  LogMinObjAlignment         = LogMinObjAlignmentInBytes - LogHeapWordSize;

  // Oop encoding heap max
  OopEncodingHeapMax = (uint64_t(max_juint) + 1) << LogMinObjAlignmentInBytes;
}

size_t Arguments::max_heap_for_compressed_oops() {
  // Avoid sign flip.
  assert(OopEncodingHeapMax > (uint64_t)os::vm_page_size(), "Unusual page size");
  // We need to fit both the null page and the heap into the memory budget, while
  // keeping alignment constraints of the heap. To guarantee the latter, as the
  // null page is located before the heap, we pad the null page to the conservative
  // maximum alignment that the GC may ever impose upon the heap.
  size_t displacement_due_to_null_page = align_up(os::vm_page_size(),
                                                  _conservative_max_heap_alignment);

  LP64_ONLY(return OopEncodingHeapMax - displacement_due_to_null_page);
  NOT_LP64(ShouldNotReachHere(); return 0);
}

void Arguments::set_use_compressed_oops() {
#ifdef _LP64
  // MaxHeapSize is not set up properly at this point, but
  // the only value that can override MaxHeapSize if we are
  // to use UseCompressedOops are InitialHeapSize and MinHeapSize.
  size_t max_heap_size = MAX3(MaxHeapSize, InitialHeapSize, MinHeapSize);

  if (max_heap_size <= max_heap_for_compressed_oops()) {
    if (FLAG_IS_DEFAULT(UseCompressedOops)) {
      FLAG_SET_ERGO(UseCompressedOops, true);
    }
  } else {
    if (UseCompressedOops && !FLAG_IS_DEFAULT(UseCompressedOops)) {
      warning("Max heap size too large for Compressed Oops");
      FLAG_SET_DEFAULT(UseCompressedOops, false);
    }
  }
#endif // _LP64
}

void Arguments::set_conservative_max_heap_alignment() {
  // The conservative maximum required alignment for the heap is the maximum of
  // the alignments imposed by several sources: any requirements from the heap
  // itself and the maximum page size we may run the VM with.
  size_t heap_alignment = GCConfig::arguments()->conservative_max_heap_alignment();
  _conservative_max_heap_alignment = MAX4(heap_alignment,
                                          os::vm_allocation_granularity(),
                                          os::max_page_size(),
                                          GCArguments::compute_heap_alignment());
}

jint Arguments::set_ergonomics_flags() {
  GCConfig::initialize();

  set_conservative_max_heap_alignment();

#ifdef _LP64
  set_use_compressed_oops();

  // Also checks that certain machines are slower with compressed oops
  // in vm_version initialization code.
#endif // _LP64

  return JNI_OK;
}

size_t Arguments::limit_heap_by_allocatable_memory(size_t limit) {
  // The AggressiveHeap check is a temporary workaround to avoid calling
  // GCarguments::heap_virtual_to_physical_ratio() before a GC has been
  // selected. This works because AggressiveHeap implies UseParallelGC
  // where we know the ratio will be 1. Once the AggressiveHeap option is
  // removed, this can be cleaned up.
  size_t heap_virtual_to_physical_ratio = (AggressiveHeap ? 1 : GCConfig::arguments()->heap_virtual_to_physical_ratio());
  size_t fraction = MaxVirtMemFraction * heap_virtual_to_physical_ratio;
  size_t max_allocatable = os::commit_memory_limit();

  return MIN2(limit, max_allocatable / fraction);
}

// Use static initialization to get the default before parsing
static const size_t DefaultHeapBaseMinAddress = HeapBaseMinAddress;

void Arguments::set_heap_size() {
  julong phys_mem;

  // If the user specified one of these options, they
  // want specific memory sizing so do not limit memory
  // based on compressed oops addressability.
  // Also, memory limits will be calculated based on
  // available os physical memory, not our MaxRAM limit,
  // unless MaxRAM is also specified.
  bool override_coop_limit = (!FLAG_IS_DEFAULT(MaxRAMPercentage) ||
                           !FLAG_IS_DEFAULT(MinRAMPercentage) ||
                           !FLAG_IS_DEFAULT(InitialRAMPercentage) ||
                           !FLAG_IS_DEFAULT(MaxRAM));
  if (override_coop_limit) {
    if (FLAG_IS_DEFAULT(MaxRAM)) {
      phys_mem = os::physical_memory();
      FLAG_SET_ERGO(MaxRAM, (uint64_t)phys_mem);
    } else {
      phys_mem = (julong)MaxRAM;
    }
  } else {
    phys_mem = FLAG_IS_DEFAULT(MaxRAM) ? MIN2(os::physical_memory(), (julong)MaxRAM)
                                       : (julong)MaxRAM;
  }

  // If the maximum heap size has not been set with -Xmx,
  // then set it as fraction of the size of physical memory,
  // respecting the maximum and minimum sizes of the heap.
  if (FLAG_IS_DEFAULT(MaxHeapSize)) {
    julong reasonable_max = (julong)(((double)phys_mem * MaxRAMPercentage) / 100);
    const julong reasonable_min = (julong)(((double)phys_mem * MinRAMPercentage) / 100);
    if (reasonable_min < MaxHeapSize) {
      // Small physical memory, so use a minimum fraction of it for the heap
      reasonable_max = reasonable_min;
    } else {
      // Not-small physical memory, so require a heap at least
      // as large as MaxHeapSize
      reasonable_max = MAX2(reasonable_max, (julong)MaxHeapSize);
    }

    if (!FLAG_IS_DEFAULT(ErgoHeapSizeLimit) && ErgoHeapSizeLimit != 0) {
      // Limit the heap size to ErgoHeapSizeLimit
      reasonable_max = MIN2(reasonable_max, (julong)ErgoHeapSizeLimit);
    }

    reasonable_max = limit_heap_by_allocatable_memory(reasonable_max);

    if (!FLAG_IS_DEFAULT(InitialHeapSize)) {
      // An initial heap size was specified on the command line,
      // so be sure that the maximum size is consistent.  Done
      // after call to limit_heap_by_allocatable_memory because that
      // method might reduce the allocation size.
      reasonable_max = MAX2(reasonable_max, (julong)InitialHeapSize);
    } else if (!FLAG_IS_DEFAULT(MinHeapSize)) {
      reasonable_max = MAX2(reasonable_max, (julong)MinHeapSize);
    }

#ifdef _LP64
    if (UseCompressedOops || UseCompressedClassPointers) {
      // HeapBaseMinAddress can be greater than default but not less than.
      if (!FLAG_IS_DEFAULT(HeapBaseMinAddress)) {
        if (HeapBaseMinAddress < DefaultHeapBaseMinAddress) {
          // matches compressed oops printing flags
          log_debug(gc, heap, coops)("HeapBaseMinAddress must be at least %zu"
                                     " (%zuG) which is greater than value given %zu",
                                     DefaultHeapBaseMinAddress,
                                     DefaultHeapBaseMinAddress/G,
                                     HeapBaseMinAddress);
          FLAG_SET_ERGO(HeapBaseMinAddress, DefaultHeapBaseMinAddress);
        }
      }
    }
    if (UseCompressedOops) {
      // Limit the heap size to the maximum possible when using compressed oops
      julong max_coop_heap = (julong)max_heap_for_compressed_oops();

      if (HeapBaseMinAddress + MaxHeapSize < max_coop_heap) {
        // Heap should be above HeapBaseMinAddress to get zero based compressed oops
        // but it should be not less than default MaxHeapSize.
        max_coop_heap -= HeapBaseMinAddress;
      }

      // If user specified flags prioritizing os physical
      // memory limits, then disable compressed oops if
      // limits exceed max_coop_heap and UseCompressedOops
      // was not specified.
      if (reasonable_max > max_coop_heap) {
        if (FLAG_IS_ERGO(UseCompressedOops) && override_coop_limit) {
          aot_log_info(aot)("UseCompressedOops disabled due to"
            " max heap %zu > compressed oop heap %zu. "
            "Please check the setting of MaxRAMPercentage %5.2f."
            ,(size_t)reasonable_max, (size_t)max_coop_heap, MaxRAMPercentage);
          FLAG_SET_ERGO(UseCompressedOops, false);
        } else {
          reasonable_max = MIN2(reasonable_max, max_coop_heap);
        }
      }
    }
#endif // _LP64

    log_trace(gc, heap)("  Maximum heap size %zu", (size_t) reasonable_max);
    FLAG_SET_ERGO(MaxHeapSize, (size_t)reasonable_max);
  }

  // If the minimum or initial heap_size have not been set or requested to be set
  // ergonomically, set them accordingly.
  if (InitialHeapSize == 0 || MinHeapSize == 0) {
    julong reasonable_minimum = (julong)(OldSize + NewSize);

    reasonable_minimum = MIN2(reasonable_minimum, (julong)MaxHeapSize);

    reasonable_minimum = limit_heap_by_allocatable_memory(reasonable_minimum);

    if (InitialHeapSize == 0) {
      julong reasonable_initial = (julong)(((double)phys_mem * InitialRAMPercentage) / 100);
      reasonable_initial = limit_heap_by_allocatable_memory(reasonable_initial);

      reasonable_initial = MAX3(reasonable_initial, reasonable_minimum, (julong)MinHeapSize);
      reasonable_initial = MIN2(reasonable_initial, (julong)MaxHeapSize);

      FLAG_SET_ERGO(InitialHeapSize, (size_t)reasonable_initial);
      log_trace(gc, heap)("  Initial heap size %zu", InitialHeapSize);
    }
    // If the minimum heap size has not been set (via -Xms or -XX:MinHeapSize),
    // synchronize with InitialHeapSize to avoid errors with the default value.
    if (MinHeapSize == 0) {
      FLAG_SET_ERGO(MinHeapSize, MIN2((size_t)reasonable_minimum, InitialHeapSize));
      log_trace(gc, heap)("  Minimum heap size %zu", MinHeapSize);
    }
  }
}

// This option inspects the machine and attempts to set various
// parameters to be optimal for long-running, memory allocation
// intensive jobs.  It is intended for machines with large
// amounts of cpu and memory.
jint Arguments::set_aggressive_heap_flags() {
  // initHeapSize is needed since _initial_heap_size is 4 bytes on a 32 bit
  // VM, but we may not be able to represent the total physical memory
  // available (like having 8gb of memory on a box but using a 32bit VM).
  // Thus, we need to make sure we're using a julong for intermediate
  // calculations.
  julong initHeapSize;
  julong total_memory = os::physical_memory();

  if (total_memory < (julong) 256 * M) {
    jio_fprintf(defaultStream::error_stream(),
            "You need at least 256mb of memory to use -XX:+AggressiveHeap\n");
    vm_exit(1);
  }

  // The heap size is half of available memory, or (at most)
  // all of possible memory less 160mb (leaving room for the OS
  // when using ISM).  This is the maximum; because adaptive sizing
  // is turned on below, the actual space used may be smaller.

  initHeapSize = MIN2(total_memory / (julong) 2,
          total_memory - (julong) 160 * M);

  initHeapSize = limit_heap_by_allocatable_memory(initHeapSize);

  if (FLAG_IS_DEFAULT(MaxHeapSize)) {
    if (FLAG_SET_CMDLINE(MaxHeapSize, initHeapSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    if (FLAG_SET_CMDLINE(InitialHeapSize, initHeapSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    if (FLAG_SET_CMDLINE(MinHeapSize, initHeapSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
  }
  if (FLAG_IS_DEFAULT(NewSize)) {
    // Make the young generation 3/8ths of the total heap.
    if (FLAG_SET_CMDLINE(NewSize,
            ((julong) MaxHeapSize / (julong) 8) * (julong) 3) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    if (FLAG_SET_CMDLINE(MaxNewSize, NewSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
  }

#if !defined(_ALLBSD_SOURCE) && !defined(AIX)  // UseLargePages is not yet supported on BSD and AIX.
  FLAG_SET_DEFAULT(UseLargePages, true);
#endif

  // Increase some data structure sizes for efficiency
  if (FLAG_SET_CMDLINE(ResizeTLAB, false) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }
  if (FLAG_SET_CMDLINE(TLABSize, 256 * K) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  // See the OldPLABSize comment below, but replace 'after promotion'
  // with 'after copying'.  YoungPLABSize is the size of the survivor
  // space per-gc-thread buffers.  The default is 4kw.
  if (FLAG_SET_CMDLINE(YoungPLABSize, 256 * K) != JVMFlag::SUCCESS) { // Note: this is in words
    return JNI_EINVAL;
  }

  // OldPLABSize is the size of the buffers in the old gen that
  // UseParallelGC uses to promote live data that doesn't fit in the
  // survivor spaces.  At any given time, there's one for each gc thread.
  // The default size is 1kw. These buffers are rarely used, since the
  // survivor spaces are usually big enough.  For specjbb, however, there
  // are occasions when there's lots of live data in the young gen
  // and we end up promoting some of it.  We don't have a definite
  // explanation for why bumping OldPLABSize helps, but the theory
  // is that a bigger PLAB results in retaining something like the
  // original allocation order after promotion, which improves mutator
  // locality.  A minor effect may be that larger PLABs reduce the
  // number of PLAB allocation events during gc.  The value of 8kw
  // was arrived at by experimenting with specjbb.
  if (FLAG_SET_CMDLINE(OldPLABSize, 8 * K) != JVMFlag::SUCCESS) { // Note: this is in words
    return JNI_EINVAL;
  }

  // Enable parallel GC and adaptive generation sizing
  if (FLAG_SET_CMDLINE(UseParallelGC, true) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  // Encourage steady state memory management
  if (FLAG_SET_CMDLINE(ThresholdTolerance, 100) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  return JNI_OK;
}

// This must be called after ergonomics.
void Arguments::set_bytecode_flags() {
  if (!RewriteBytecodes) {
    FLAG_SET_DEFAULT(RewriteFrequentPairs, false);
  }
}

// Aggressive optimization flags
jint Arguments::set_aggressive_opts_flags() {
#ifdef COMPILER2
  if (AggressiveUnboxing) {
    if (FLAG_IS_DEFAULT(EliminateAutoBox)) {
      FLAG_SET_DEFAULT(EliminateAutoBox, true);
    } else if (!EliminateAutoBox) {
      // warning("AggressiveUnboxing is disabled because EliminateAutoBox is disabled");
      AggressiveUnboxing = false;
    }
    if (FLAG_IS_DEFAULT(DoEscapeAnalysis)) {
      FLAG_SET_DEFAULT(DoEscapeAnalysis, true);
    } else if (!DoEscapeAnalysis) {
      // warning("AggressiveUnboxing is disabled because DoEscapeAnalysis is disabled");
      AggressiveUnboxing = false;
    }
  }
  if (!FLAG_IS_DEFAULT(AutoBoxCacheMax)) {
    if (FLAG_IS_DEFAULT(EliminateAutoBox)) {
      FLAG_SET_DEFAULT(EliminateAutoBox, true);
    }
    // Feed the cache size setting into the JDK
    char buffer[1024];
    jio_snprintf(buffer, 1024, "java.lang.Integer.IntegerCache.high=%zd", AutoBoxCacheMax);
    if (!add_property(buffer)) {
      return JNI_ENOMEM;
    }
  }
#endif

  return JNI_OK;
}

//===========================================================================================================

void Arguments::process_java_launcher_argument(const char* launcher, void* extra_info) {
  if (_sun_java_launcher != _default_java_launcher) {
    os::free(const_cast<char*>(_sun_java_launcher));
  }
  _sun_java_launcher = os::strdup_check_oom(launcher);
}

bool Arguments::created_by_java_launcher() {
  assert(_sun_java_launcher != nullptr, "property must have value");
  return strcmp(DEFAULT_JAVA_LAUNCHER, _sun_java_launcher) != 0;
}

bool Arguments::executing_unit_tests() {
  return _executing_unit_tests;
}

//===========================================================================================================
// Parsing of main arguments

static unsigned int addreads_count = 0;
static unsigned int addexports_count = 0;
static unsigned int addopens_count = 0;
static unsigned int patch_mod_count = 0;
static unsigned int enable_native_access_count = 0;
static bool patch_mod_javabase = false;

// Check the consistency of vm_init_args
bool Arguments::check_vm_args_consistency() {
  // This may modify compiler flags. Must be called before CompilerConfig::check_args_consistency()
  if (!CDSConfig::check_vm_args_consistency(patch_mod_javabase, mode_flag_cmd_line)) {
    return false;
  }

  // Method for adding checks for flag consistency.
  // The intent is to warn the user of all possible conflicts,
  // before returning an error.
  // Note: Needs platform-dependent factoring.
  bool status = true;

  if (TLABRefillWasteFraction == 0) {
    jio_fprintf(defaultStream::error_stream(),
                "TLABRefillWasteFraction should be a denominator, "
                "not %zu\n",
                TLABRefillWasteFraction);
    status = false;
  }

  status = CompilerConfig::check_args_consistency(status);
#if INCLUDE_JVMCI
  if (status && EnableJVMCI) {
    // Add the JVMCI module if not using libjvmci or EnableJVMCI
    // was explicitly set on the command line or in the jimage.
    if ((!UseJVMCINativeLibrary || FLAG_IS_CMDLINE(EnableJVMCI) || FLAG_IS_JIMAGE_RESOURCE(EnableJVMCI)) && ClassLoader::is_module_observable("jdk.internal.vm.ci") && !_jvmci_module_added) {
      if (!create_numbered_module_property("jdk.module.addmods", "jdk.internal.vm.ci", _addmods_count++)) {
        return false;
      }
    }
  }
#endif

#if INCLUDE_JFR
  if (status && (FlightRecorderOptions || StartFlightRecording)) {
    if (!create_numbered_module_property("jdk.module.addmods", "jdk.jfr", _addmods_count++)) {
      return false;
    }
  }
#endif

#ifndef SUPPORT_RESERVED_STACK_AREA
  if (StackReservedPages != 0) {
    FLAG_SET_CMDLINE(StackReservedPages, 0);
    warning("Reserved Stack Area not supported on this platform");
  }
#endif

  if (UseObjectMonitorTable && LockingMode != LM_LIGHTWEIGHT) {
    // ObjectMonitorTable requires lightweight locking.
    FLAG_SET_CMDLINE(UseObjectMonitorTable, false);
    warning("UseObjectMonitorTable requires LM_LIGHTWEIGHT");
  }

#if !defined(X86) && !defined(AARCH64) && !defined(PPC64) && !defined(RISCV64) && !defined(S390)
  if (LockingMode == LM_MONITOR) {
    jio_fprintf(defaultStream::error_stream(),
                "LockingMode == 0 (LM_MONITOR) is not fully implemented on this architecture\n");
    return false;
  }
#endif
  if (VerifyHeavyMonitors && LockingMode != LM_MONITOR) {
    jio_fprintf(defaultStream::error_stream(),
                "-XX:+VerifyHeavyMonitors requires LockingMode == 0 (LM_MONITOR)\n");
    return false;
  }
  return status;
}

bool Arguments::is_bad_option(const JavaVMOption* option, jboolean ignore,
  const char* option_type) {
  if (ignore) return false;

  const char* spacer = " ";
  if (option_type == nullptr) {
    option_type = ++spacer; // Set both to the empty string.
  }

  jio_fprintf(defaultStream::error_stream(),
              "Unrecognized %s%soption: %s\n", option_type, spacer,
              option->optionString);
  return true;
}

static const char* user_assertion_options[] = {
  "-da", "-ea", "-disableassertions", "-enableassertions", nullptr
};

static const char* system_assertion_options[] = {
  "-dsa", "-esa", "-disablesystemassertions", "-enablesystemassertions", nullptr
};

bool Arguments::parse_uint(const char* value,
                           uint* uint_arg,
                           uint min_size) {
  uint n;
  if (!parse_integer(value, &n)) {
    return false;
  }
  if (n >= min_size) {
    *uint_arg = n;
    return true;
  } else {
    return false;
  }
}

bool Arguments::create_module_property(const char* prop_name, const char* prop_value, PropertyInternal internal) {
  assert(is_internal_module_property(prop_name), "unknown module property: '%s'", prop_name);
  CDSConfig::check_internal_module_property(prop_name, prop_value);
  size_t prop_len = strlen(prop_name) + strlen(prop_value) + 2;
  char* property = AllocateHeap(prop_len, mtArguments);
  int ret = jio_snprintf(property, prop_len, "%s=%s", prop_name, prop_value);
  if (ret < 0 || ret >= (int)prop_len) {
    FreeHeap(property);
    return false;
  }
  // These are not strictly writeable properties as they cannot be set via -Dprop=val. But that
  // is enforced by checking is_internal_module_property(). We need the property to be writeable so
  // that multiple occurrences of the associated flag just causes the existing property value to be
  // replaced ("last option wins"). Otherwise we would need to keep track of the flags and only convert
  // to a property after we have finished flag processing.
  bool added = add_property(property, WriteableProperty, internal);
  FreeHeap(property);
  return added;
}

bool Arguments::create_numbered_module_property(const char* prop_base_name, const char* prop_value, unsigned int count) {
  assert(is_internal_module_property(prop_base_name), "unknown module property: '%s'", prop_base_name);
  CDSConfig::check_internal_module_property(prop_base_name, prop_value);
  const unsigned int props_count_limit = 1000;
  const int max_digits = 3;
  const int extra_symbols_count = 3; // includes '.', '=', '\0'

  // Make sure count is < props_count_limit. Otherwise, memory allocation will be too small.
  if (count < props_count_limit) {
    size_t prop_len = strlen(prop_base_name) + strlen(prop_value) + max_digits + extra_symbols_count;
    char* property = AllocateHeap(prop_len, mtArguments);
    int ret = jio_snprintf(property, prop_len, "%s.%d=%s", prop_base_name, count, prop_value);
    if (ret < 0 || ret >= (int)prop_len) {
      FreeHeap(property);
      jio_fprintf(defaultStream::error_stream(), "Failed to create property %s.%d=%s\n", prop_base_name, count, prop_value);
      return false;
    }
    bool added = add_property(property, UnwriteableProperty, InternalProperty);
    FreeHeap(property);
    return added;
  }

  jio_fprintf(defaultStream::error_stream(), "Property count limit exceeded: %s, limit=%d\n", prop_base_name, props_count_limit);
  return false;
}

Arguments::ArgsRange Arguments::parse_memory_size(const char* s,
                                                  julong* long_arg,
                                                  julong min_size,
                                                  julong max_size) {
  if (!parse_integer(s, long_arg)) return arg_unreadable;
  return check_memory_size(*long_arg, min_size, max_size);
}

jint Arguments::parse_vm_init_args(GrowableArrayCHeap<VMInitArgsGroup, mtArguments>* all_args) {
  // Save default settings for some mode flags
  Arguments::_AlwaysCompileLoopMethods = AlwaysCompileLoopMethods;
  Arguments::_UseOnStackReplacement    = UseOnStackReplacement;
  Arguments::_ClipInlining             = ClipInlining;
  Arguments::_BackgroundCompilation    = BackgroundCompilation;

  // Remember the default value of SharedBaseAddress.
  Arguments::_default_SharedBaseAddress = SharedBaseAddress;

  // Setup flags for mixed which is the default
  set_mode_flags(_mixed);

  jint result;
  for (int i = 0; i < all_args->length(); i++) {
    result = parse_each_vm_init_arg(all_args->at(i)._args, all_args->at(i)._origin);
    if (result != JNI_OK) {
      return result;
    }
  }

  // Disable CDS for exploded image
  if (!has_jimage()) {
    no_shared_spaces("CDS disabled on exploded JDK");
  }

  // We need to ensure processor and memory resources have been properly
  // configured - which may rely on arguments we just processed - before
  // doing the final argument processing. Any argument processing that
  // needs to know about processor and memory resources must occur after
  // this point.

  os::init_container_support();

  SystemMemoryBarrier::initialize();

  // Do final processing now that all arguments have been parsed
  result = finalize_vm_init_args();
  if (result != JNI_OK) {
    return result;
  }

  return JNI_OK;
}

#if !INCLUDE_JVMTI || INCLUDE_CDS
// Checks if name in command-line argument -agent{lib,path}:name[=options]
// represents a valid JDWP agent.  is_path==true denotes that we
// are dealing with -agentpath (case where name is a path), otherwise with
// -agentlib
static bool valid_jdwp_agent(char *name, bool is_path) {
  char *_name;
  const char *_jdwp = "jdwp";
  size_t _len_jdwp, _len_prefix;

  if (is_path) {
    if ((_name = strrchr(name, (int) *os::file_separator())) == nullptr) {
      return false;
    }

    _name++;  // skip past last path separator
    _len_prefix = strlen(JNI_LIB_PREFIX);

    if (strncmp(_name, JNI_LIB_PREFIX, _len_prefix) != 0) {
      return false;
    }

    _name += _len_prefix;
    _len_jdwp = strlen(_jdwp);

    if (strncmp(_name, _jdwp, _len_jdwp) == 0) {
      _name += _len_jdwp;
    }
    else {
      return false;
    }

    if (strcmp(_name, JNI_LIB_SUFFIX) != 0) {
      return false;
    }

    return true;
  }

  if (strcmp(name, _jdwp) == 0) {
    return true;
  }

  return false;
}
#endif

int Arguments::process_patch_mod_option(const char* patch_mod_tail) {
  // --patch-module=<module>=<file>(<pathsep><file>)*
  assert(patch_mod_tail != nullptr, "Unexpected null patch-module value");
  // Find the equal sign between the module name and the path specification
  const char* module_equal = strchr(patch_mod_tail, '=');
  if (module_equal == nullptr) {
    jio_fprintf(defaultStream::output_stream(), "Missing '=' in --patch-module specification\n");
    return JNI_ERR;
  } else {
    // Pick out the module name
    size_t module_len = module_equal - patch_mod_tail;
    char* module_name = NEW_C_HEAP_ARRAY_RETURN_NULL(char, module_len+1, mtArguments);
    if (module_name != nullptr) {
      memcpy(module_name, patch_mod_tail, module_len);
      *(module_name + module_len) = '\0';
      // The path piece begins one past the module_equal sign
      add_patch_mod_prefix(module_name, module_equal + 1);
      FREE_C_HEAP_ARRAY(char, module_name);
      if (!create_numbered_module_property("jdk.module.patch", patch_mod_tail, patch_mod_count++)) {
        return JNI_ENOMEM;
      }
    } else {
      return JNI_ENOMEM;
    }
  }
  return JNI_OK;
}

// Parse -Xss memory string parameter and convert to ThreadStackSize in K.
jint Arguments::parse_xss(const JavaVMOption* option, const char* tail, intx* out_ThreadStackSize) {
  // The min and max sizes match the values in globals.hpp, but scaled
  // with K. The values have been chosen so that alignment with page
  // size doesn't change the max value, which makes the conversions
  // back and forth between Xss value and ThreadStackSize value easier.
  // The values have also been chosen to fit inside a 32-bit signed type.
  const julong min_ThreadStackSize = 0;
  const julong max_ThreadStackSize = 1 * M;

  // Make sure the above values match the range set in globals.hpp
  const JVMTypedFlagLimit<intx>* limit = JVMFlagLimit::get_range_at(FLAG_MEMBER_ENUM(ThreadStackSize))->cast<intx>();
  assert(min_ThreadStackSize == static_cast<julong>(limit->min()), "must be");
  assert(max_ThreadStackSize == static_cast<julong>(limit->max()), "must be");

  const julong min_size = min_ThreadStackSize * K;
  const julong max_size = max_ThreadStackSize * K;

  assert(is_aligned(max_size, os::vm_page_size()), "Implementation assumption");

  julong size = 0;
  ArgsRange errcode = parse_memory_size(tail, &size, min_size, max_size);
  if (errcode != arg_in_range) {
    bool silent = (option == nullptr); // Allow testing to silence error messages
    if (!silent) {
      jio_fprintf(defaultStream::error_stream(),
                  "Invalid thread stack size: %s\n", option->optionString);
      describe_range_error(errcode);
    }
    return JNI_EINVAL;
  }

  // Internally track ThreadStackSize in units of 1024 bytes.
  const julong size_aligned = align_up(size, K);
  assert(size <= size_aligned,
         "Overflow: " JULONG_FORMAT " " JULONG_FORMAT,
         size, size_aligned);

  const julong size_in_K = size_aligned / K;
  assert(size_in_K < (julong)max_intx,
         "size_in_K doesn't fit in the type of ThreadStackSize: " JULONG_FORMAT,
         size_in_K);

  // Check that code expanding ThreadStackSize to a page aligned number of bytes won't overflow.
  const julong max_expanded = align_up(size_in_K * K, os::vm_page_size());
  assert(max_expanded < max_uintx && max_expanded >= size_in_K,
         "Expansion overflowed: " JULONG_FORMAT " " JULONG_FORMAT,
         max_expanded, size_in_K);

  *out_ThreadStackSize = (intx)size_in_K;

  return JNI_OK;
}

jint Arguments::parse_each_vm_init_arg(const JavaVMInitArgs* args, JVMFlagOrigin origin) {
  // For match_option to return remaining or value part of option string
  const char* tail;

  // iterate over arguments
  for (int index = 0; index < args->nOptions; index++) {
    bool is_absolute_path = false;  // for -agentpath vs -agentlib

    const JavaVMOption* option = args->options + index;

    if (!match_option(option, "-Djava.class.path", &tail) &&
        !match_option(option, "-Dsun.java.command", &tail) &&
        !match_option(option, "-Dsun.java.launcher", &tail)) {

        // add all jvm options to the jvm_args string. This string
        // is used later to set the java.vm.args PerfData string constant.
        // the -Djava.class.path and the -Dsun.java.command options are
        // omitted from jvm_args string as each have their own PerfData
        // string constant object.
        build_jvm_args(option->optionString);
    }

    // -verbose:[class/module/gc/jni]
    if (match_option(option, "-verbose", &tail)) {
      if (!strcmp(tail, ":class") || !strcmp(tail, "")) {
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(class, load));
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(class, unload));
      } else if (!strcmp(tail, ":module")) {
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(module, load));
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(module, unload));
      } else if (!strcmp(tail, ":gc")) {
        if (_legacyGCLogging.lastFlag == 0) {
          _legacyGCLogging.lastFlag = 1;
        }
      } else if (!strcmp(tail, ":jni")) {
        LogConfiguration::configure_stdout(LogLevel::Debug, true, LOG_TAGS(jni, resolve));
      }
    // -da / -ea / -disableassertions / -enableassertions
    // These accept an optional class/package name separated by a colon, e.g.,
    // -da:java.lang.Thread.
    } else if (match_option(option, user_assertion_options, &tail, true)) {
      bool enable = option->optionString[1] == 'e';     // char after '-' is 'e'
      if (*tail == '\0') {
        JavaAssertions::setUserClassDefault(enable);
      } else {
        assert(*tail == ':', "bogus match by match_option()");
        JavaAssertions::addOption(tail + 1, enable);
      }
    // -dsa / -esa / -disablesystemassertions / -enablesystemassertions
    } else if (match_option(option, system_assertion_options, &tail, false)) {
      bool enable = option->optionString[1] == 'e';     // char after '-' is 'e'
      JavaAssertions::setSystemClassDefault(enable);
    // -bootclasspath:
    } else if (match_option(option, "-Xbootclasspath:", &tail)) {
        jio_fprintf(defaultStream::output_stream(),
          "-Xbootclasspath is no longer a supported option.\n");
        return JNI_EINVAL;
    // -bootclasspath/a:
    } else if (match_option(option, "-Xbootclasspath/a:", &tail)) {
      Arguments::append_sysclasspath(tail);
    // -bootclasspath/p:
    } else if (match_option(option, "-Xbootclasspath/p:", &tail)) {
        jio_fprintf(defaultStream::output_stream(),
          "-Xbootclasspath/p is no longer a supported option.\n");
        return JNI_EINVAL;
    // -Xrun
    } else if (match_option(option, "-Xrun", &tail)) {
      if (tail != nullptr) {
        const char* pos = strchr(tail, ':');
        size_t len = (pos == nullptr) ? strlen(tail) : pos - tail;
        char* name = NEW_C_HEAP_ARRAY(char, len + 1, mtArguments);
        jio_snprintf(name, len + 1, "%s", tail);

        char *options = nullptr;
        if(pos != nullptr) {
          size_t len2 = strlen(pos+1) + 1; // options start after ':'.  Final zero must be copied.
          options = (char*)memcpy(NEW_C_HEAP_ARRAY(char, len2, mtArguments), pos+1, len2);
        }
#if !INCLUDE_JVMTI
        if (strcmp(name, "jdwp") == 0) {
          jio_fprintf(defaultStream::error_stream(),
            "Debugging agents are not supported in this VM\n");
          return JNI_ERR;
        }
#endif // !INCLUDE_JVMTI
        JvmtiAgentList::add_xrun(name, options, false);
        FREE_C_HEAP_ARRAY(char, name);
        FREE_C_HEAP_ARRAY(char, options);
      }
    } else if (match_option(option, "--add-reads=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addreads", tail, addreads_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--add-exports=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addexports", tail, addexports_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--add-opens=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addopens", tail, addopens_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--add-modules=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addmods", tail, _addmods_count++)) {
        return JNI_ENOMEM;
      }
#if INCLUDE_JVMCI
      if (!_jvmci_module_added) {
        const char *jvmci_module = strstr(tail, "jdk.internal.vm.ci");
        if (jvmci_module != nullptr) {
          char before = *(jvmci_module - 1);
          char after  = *(jvmci_module + strlen("jdk.internal.vm.ci"));
          if ((before == '=' || before == ',') && (after == '\0' || after == ',')) {
            FLAG_SET_DEFAULT(EnableJVMCI, true);
            _jvmci_module_added = true;
          }
        }
      }
#endif
    } else if (match_option(option, "--enable-native-access=", &tail)) {
      if (!create_numbered_module_property("jdk.module.enable.native.access", tail, enable_native_access_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--illegal-native-access=", &tail)) {
      if (!create_module_property("jdk.module.illegal.native.access", tail, InternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--limit-modules=", &tail)) {
      if (!create_module_property("jdk.module.limitmods", tail, InternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--module-path=", &tail)) {
      if (!create_module_property("jdk.module.path", tail, ExternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--upgrade-module-path=", &tail)) {
      if (!create_module_property("jdk.module.upgrade.path", tail, ExternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--patch-module=", &tail)) {
      // --patch-module=<module>=<file>(<pathsep><file>)*
      int res = process_patch_mod_option(tail);
      if (res != JNI_OK) {
        return res;
      }
    } else if (match_option(option, "--sun-misc-unsafe-memory-access=", &tail)) {
      if (strcmp(tail, "allow") == 0 || strcmp(tail, "warn") == 0 || strcmp(tail, "debug") == 0 || strcmp(tail, "deny") == 0) {
        PropertyList_unique_add(&_system_properties, "sun.misc.unsafe.memory.access", tail,
                                AddProperty, WriteableProperty, InternalProperty);
      } else {
        jio_fprintf(defaultStream::error_stream(),
                    "Value specified to --sun-misc-unsafe-memory-access not recognized: '%s'\n", tail);
        return JNI_ERR;
      }
    } else if (match_option(option, "--illegal-access=", &tail)) {
      char version[256];
      JDK_Version::jdk(17).to_string(version, sizeof(version));
      warning("Ignoring option %s; support was removed in %s", option->optionString, version);
    // -agentlib and -agentpath
    } else if (match_option(option, "-agentlib:", &tail) ||
          (is_absolute_path = match_option(option, "-agentpath:", &tail))) {
      if(tail != nullptr) {
        const char* pos = strchr(tail, '=');
        char* name;
        if (pos == nullptr) {
          name = os::strdup_check_oom(tail, mtArguments);
        } else {
          size_t len = pos - tail;
          name = NEW_C_HEAP_ARRAY(char, len + 1, mtArguments);
          memcpy(name, tail, len);
          name[len] = '\0';
        }

        char *options = nullptr;
        if(pos != nullptr) {
          options = os::strdup_check_oom(pos + 1, mtArguments);
        }
#if !INCLUDE_JVMTI
        if (valid_jdwp_agent(name, is_absolute_path)) {
          jio_fprintf(defaultStream::error_stream(),
            "Debugging agents are not supported in this VM\n");
          return JNI_ERR;
        }
#elif INCLUDE_CDS
        if (valid_jdwp_agent(name, is_absolute_path)) {
          _has_jdwp_agent = true;
        }
#endif // !INCLUDE_JVMTI
        JvmtiAgentList::add(name, options, is_absolute_path);
        os::free(name);
        os::free(options);
      }
    // -javaagent
    } else if (match_option(option, "-javaagent:", &tail)) {
#if !INCLUDE_JVMTI
      jio_fprintf(defaultStream::error_stream(),
        "Instrumentation agents are not supported in this VM\n");
      return JNI_ERR;
#else
      if (tail != nullptr) {
        size_t length = strlen(tail) + 1;
        char *options = NEW_C_HEAP_ARRAY(char, length, mtArguments);
        jio_snprintf(options, length, "%s", tail);
        JvmtiAgentList::add("instrument", options, false);
        FREE_C_HEAP_ARRAY(char, options);

        // java agents need module java.instrument
        if (!create_numbered_module_property("jdk.module.addmods", "java.instrument", _addmods_count++)) {
          return JNI_ENOMEM;
        }
      }
#endif // !INCLUDE_JVMTI
    // --enable_preview
    } else if (match_option(option, "--enable-preview")) {
      set_enable_preview();
    // -Xnoclassgc
    } else if (match_option(option, "-Xnoclassgc")) {
      if (FLAG_SET_CMDLINE(ClassUnloading, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xbatch
    } else if (match_option(option, "-Xbatch")) {
      if (FLAG_SET_CMDLINE(BackgroundCompilation, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xmn for compatibility with other JVM vendors
    } else if (match_option(option, "-Xmn", &tail)) {
      julong long_initial_young_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_initial_young_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid initial young generation size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(MaxNewSize, (size_t)long_initial_young_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(NewSize, (size_t)long_initial_young_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xms
    } else if (match_option(option, "-Xms", &tail)) {
      julong size = 0;
      // an initial heap size of 0 means automatically determine
      ArgsRange errcode = parse_memory_size(tail, &size, 0);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid initial heap size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(MinHeapSize, (size_t)size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(InitialHeapSize, (size_t)size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xmx
    } else if (match_option(option, "-Xmx", &tail) || match_option(option, "-XX:MaxHeapSize=", &tail)) {
      julong long_max_heap_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_max_heap_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum heap size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(MaxHeapSize, (size_t)long_max_heap_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // Xmaxf
    } else if (match_option(option, "-Xmaxf", &tail)) {
      char* err;
      int maxf = (int)(strtod(tail, &err) * 100);
      if (*err != '\0' || *tail == '\0') {
        jio_fprintf(defaultStream::error_stream(),
                    "Bad max heap free percentage size: %s\n",
                    option->optionString);
        return JNI_EINVAL;
      } else {
        if (FLAG_SET_CMDLINE(MaxHeapFreeRatio, maxf) != JVMFlag::SUCCESS) {
            return JNI_EINVAL;
        }
      }
    // Xminf
    } else if (match_option(option, "-Xminf", &tail)) {
      char* err;
      int minf = (int)(strtod(tail, &err) * 100);
      if (*err != '\0' || *tail == '\0') {
        jio_fprintf(defaultStream::error_stream(),
                    "Bad min heap free percentage size: %s\n",
                    option->optionString);
        return JNI_EINVAL;
      } else {
        if (FLAG_SET_CMDLINE(MinHeapFreeRatio, minf) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      }
    // -Xss
    } else if (match_option(option, "-Xss", &tail)) {
      intx value = 0;
      jint err = parse_xss(option, tail, &value);
      if (err != JNI_OK) {
        return err;
      }
      if (FLAG_SET_CMDLINE(ThreadStackSize, value) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-Xmaxjitcodesize", &tail) ||
               match_option(option, "-XX:ReservedCodeCacheSize=", &tail)) {
      julong long_ReservedCodeCacheSize = 0;

      ArgsRange errcode = parse_memory_size(tail, &long_ReservedCodeCacheSize, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum code cache size: %s.\n", option->optionString);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(ReservedCodeCacheSize, (size_t)long_ReservedCodeCacheSize) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -green
    } else if (match_option(option, "-green")) {
      jio_fprintf(defaultStream::error_stream(),
                  "Green threads support not available\n");
          return JNI_EINVAL;
    // -native
    } else if (match_option(option, "-native")) {
          // HotSpot always uses native threads, ignore silently for compatibility
    // -Xrs
    } else if (match_option(option, "-Xrs")) {
          // Classic/EVM option, new functionality
      if (FLAG_SET_CMDLINE(ReduceSignalUsage, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      // -Xprof
    } else if (match_option(option, "-Xprof")) {
      char version[256];
      // Obsolete in JDK 10
      JDK_Version::jdk(10).to_string(version, sizeof(version));
      warning("Ignoring option %s; support was removed in %s", option->optionString, version);
    // -Xinternalversion
    } else if (match_option(option, "-Xinternalversion")) {
      jio_fprintf(defaultStream::output_stream(), "%s\n",
                  VM_Version::internal_vm_info_string());
      vm_exit(0);
#ifndef PRODUCT
    // -Xprintflags
    } else if (match_option(option, "-Xprintflags")) {
      JVMFlag::printFlags(tty, false);
      vm_exit(0);
#endif
    // -D
    } else if (match_option(option, "-D", &tail)) {
      const char* value;
      if (match_option(option, "-Djava.endorsed.dirs=", &value) &&
            *value!= '\0' && strcmp(value, "\"\"") != 0) {
        // abort if -Djava.endorsed.dirs is set
        jio_fprintf(defaultStream::output_stream(),
          "-Djava.endorsed.dirs=%s is not supported. Endorsed standards and standalone APIs\n"
          "in modular form will be supported via the concept of upgradeable modules.\n", value);
        return JNI_EINVAL;
      }
      if (match_option(option, "-Djava.ext.dirs=", &value) &&
            *value != '\0' && strcmp(value, "\"\"") != 0) {
        // abort if -Djava.ext.dirs is set
        jio_fprintf(defaultStream::output_stream(),
          "-Djava.ext.dirs=%s is not supported.  Use -classpath instead.\n", value);
        return JNI_EINVAL;
      }
      // Check for module related properties.  They must be set using the modules
      // options. For example: use "--add-modules=java.sql", not
      // "-Djdk.module.addmods=java.sql"
      if (is_internal_module_property(option->optionString + 2)) {
        needs_module_property_warning = true;
        continue;
      }
      if (!add_property(tail)) {
        return JNI_ENOMEM;
      }
      // Out of the box management support
      if (match_option(option, "-Dcom.sun.management", &tail)) {
#if INCLUDE_MANAGEMENT
        if (FLAG_SET_CMDLINE(ManagementServer, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        // management agent in module jdk.management.agent
        if (!create_numbered_module_property("jdk.module.addmods", "jdk.management.agent", _addmods_count++)) {
          return JNI_ENOMEM;
        }
#else
        jio_fprintf(defaultStream::output_stream(),
          "-Dcom.sun.management is not supported in this VM.\n");
        return JNI_ERR;
#endif
      }
    // -Xint
    } else if (match_option(option, "-Xint")) {
          set_mode_flags(_int);
          mode_flag_cmd_line = true;
    // -Xmixed
    } else if (match_option(option, "-Xmixed")) {
          set_mode_flags(_mixed);
          mode_flag_cmd_line = true;
    // -Xcomp
    } else if (match_option(option, "-Xcomp")) {
      // for testing the compiler; turn off all flags that inhibit compilation
          set_mode_flags(_comp);
          mode_flag_cmd_line = true;
    // -Xshare:dump
    } else if (match_option(option, "-Xshare:dump")) {
      CDSConfig::enable_dumping_static_archive();
      CDSConfig::set_old_cds_flags_used();
    // -Xshare:on
    } else if (match_option(option, "-Xshare:on")) {
      UseSharedSpaces = true;
      RequireSharedSpaces = true;
      CDSConfig::set_old_cds_flags_used();
    // -Xshare:auto || -XX:ArchiveClassesAtExit=<archive file>
    } else if (match_option(option, "-Xshare:auto")) {
      UseSharedSpaces = true;
      RequireSharedSpaces = false;
      xshare_auto_cmd_line = true;
      CDSConfig::set_old_cds_flags_used();
    // -Xshare:off
    } else if (match_option(option, "-Xshare:off")) {
      UseSharedSpaces = false;
      RequireSharedSpaces = false;
      CDSConfig::set_old_cds_flags_used();
    // -Xverify
    } else if (match_option(option, "-Xverify", &tail)) {
      if (strcmp(tail, ":all") == 0 || strcmp(tail, "") == 0) {
        if (FLAG_SET_CMDLINE(BytecodeVerificationLocal, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(BytecodeVerificationRemote, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else if (strcmp(tail, ":remote") == 0) {
        if (FLAG_SET_CMDLINE(BytecodeVerificationLocal, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(BytecodeVerificationRemote, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else if (strcmp(tail, ":none") == 0) {
        if (FLAG_SET_CMDLINE(BytecodeVerificationLocal, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(BytecodeVerificationRemote, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        warning("Options -Xverify:none and -noverify were deprecated in JDK 13 and will likely be removed in a future release.");
      } else if (is_bad_option(option, args->ignoreUnrecognized, "verification")) {
        return JNI_EINVAL;
      }
    // -Xdebug
    } else if (match_option(option, "-Xdebug")) {
      warning("Option -Xdebug was deprecated in JDK 22 and will likely be removed in a future release.");
    } else if (match_option(option, "-Xloggc:", &tail)) {
      // Deprecated flag to redirect GC output to a file. -Xloggc:<filename>
      log_warning(gc)("-Xloggc is deprecated. Will use -Xlog:gc:%s instead.", tail);
      _legacyGCLogging.lastFlag = 2;
      _legacyGCLogging.file = os::strdup_check_oom(tail);
    } else if (match_option(option, "-Xlog", &tail)) {
      bool ret = false;
      if (strcmp(tail, ":help") == 0) {
        fileStream stream(defaultStream::output_stream());
        LogConfiguration::print_command_line_help(&stream);
        vm_exit(0);
      } else if (strcmp(tail, ":disable") == 0) {
        LogConfiguration::disable_logging();
        ret = true;
      } else if (strncmp(tail, ":async", strlen(":async")) == 0) {
        const char* async_tail = tail + strlen(":async");
        ret = LogConfiguration::parse_async_argument(async_tail);
      } else if (*tail == '\0') {
        ret = LogConfiguration::parse_command_line_arguments();
        assert(ret, "-Xlog without arguments should never fail to parse");
      } else if (*tail == ':') {
        ret = LogConfiguration::parse_command_line_arguments(tail + 1);
      }
      if (ret == false) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid -Xlog option '-Xlog%s', see error log for details.\n",
                    tail);
        return JNI_EINVAL;
      }
    // JNI hooks
    } else if (match_option(option, "-Xcheck", &tail)) {
      if (!strcmp(tail, ":jni")) {
#if !INCLUDE_JNI_CHECK
        warning("JNI CHECKING is not supported in this VM");
#else
        CheckJNICalls = true;
#endif // INCLUDE_JNI_CHECK
      } else if (is_bad_option(option, args->ignoreUnrecognized,
                                     "check")) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "vfprintf")) {
      _vfprintf_hook = CAST_TO_FN_PTR(vfprintf_hook_t, option->extraInfo);
    } else if (match_option(option, "exit")) {
      _exit_hook = CAST_TO_FN_PTR(exit_hook_t, option->extraInfo);
    } else if (match_option(option, "abort")) {
      _abort_hook = CAST_TO_FN_PTR(abort_hook_t, option->extraInfo);
    // Need to keep consistency of MaxTenuringThreshold and AlwaysTenure/NeverTenure;
    // and the last option wins.
    } else if (match_option(option, "-XX:+NeverTenure")) {
      if (FLAG_SET_CMDLINE(NeverTenure, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(AlwaysTenure, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(MaxTenuringThreshold, markWord::max_age + 1) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+AlwaysTenure")) {
      if (FLAG_SET_CMDLINE(NeverTenure, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(AlwaysTenure, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(MaxTenuringThreshold, 0) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:MaxTenuringThreshold=", &tail)) {
      uint max_tenuring_thresh = 0;
      if (!parse_uint(tail, &max_tenuring_thresh, 0)) {
        jio_fprintf(defaultStream::error_stream(),
                    "Improperly specified VM option \'MaxTenuringThreshold=%s\'\n", tail);
        return JNI_EINVAL;
      }

      if (FLAG_SET_CMDLINE(MaxTenuringThreshold, max_tenuring_thresh) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }

      if (MaxTenuringThreshold == 0) {
        if (FLAG_SET_CMDLINE(NeverTenure, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(AlwaysTenure, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else {
        if (FLAG_SET_CMDLINE(NeverTenure, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(AlwaysTenure, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      }
    } else if (match_option(option, "-XX:+DisplayVMOutputToStderr")) {
      if (FLAG_SET_CMDLINE(DisplayVMOutputToStdout, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(DisplayVMOutputToStderr, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+DisplayVMOutputToStdout")) {
      if (FLAG_SET_CMDLINE(DisplayVMOutputToStderr, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(DisplayVMOutputToStdout, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+ErrorFileToStderr")) {
      if (FLAG_SET_CMDLINE(ErrorFileToStdout, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(ErrorFileToStderr, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+ErrorFileToStdout")) {
      if (FLAG_SET_CMDLINE(ErrorFileToStderr, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(ErrorFileToStdout, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "--finalization=", &tail)) {
      if (strcmp(tail, "enabled") == 0) {
        InstanceKlass::set_finalization_enabled(true);
      } else if (strcmp(tail, "disabled") == 0) {
        InstanceKlass::set_finalization_enabled(false);
      } else {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid finalization value '%s', must be 'disabled' or 'enabled'.\n",
                    tail);
        return JNI_EINVAL;
      }
#if !defined(DTRACE_ENABLED)
    } else if (match_option(option, "-XX:+DTraceMethodProbes")) {
      jio_fprintf(defaultStream::error_stream(),
                  "DTraceMethodProbes flag is not applicable for this configuration\n");
      return JNI_EINVAL;
    } else if (match_option(option, "-XX:+DTraceAllocProbes")) {
      jio_fprintf(defaultStream::error_stream(),
                  "DTraceAllocProbes flag is not applicable for this configuration\n");
      return JNI_EINVAL;
    } else if (match_option(option, "-XX:+DTraceMonitorProbes")) {
      jio_fprintf(defaultStream::error_stream(),
                  "DTraceMonitorProbes flag is not applicable for this configuration\n");
      return JNI_EINVAL;
#endif // !defined(DTRACE_ENABLED)
#ifdef ASSERT
    } else if (match_option(option, "-XX:+FullGCALot")) {
      if (FLAG_SET_CMDLINE(FullGCALot, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
#endif
#if !INCLUDE_MANAGEMENT
    } else if (match_option(option, "-XX:+ManagementServer")) {
        jio_fprintf(defaultStream::error_stream(),
          "ManagementServer is not supported in this VM.\n");
        return JNI_ERR;
#endif // INCLUDE_MANAGEMENT
#if INCLUDE_JVMCI
    } else if (match_option(option, "-XX:-EnableJVMCIProduct") || match_option(option, "-XX:-UseGraalJIT")) {
      if (EnableJVMCIProduct) {
        jio_fprintf(defaultStream::error_stream(),
                  "-XX:-EnableJVMCIProduct or -XX:-UseGraalJIT cannot come after -XX:+EnableJVMCIProduct or -XX:+UseGraalJIT\n");
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+EnableJVMCIProduct") || match_option(option, "-XX:+UseGraalJIT")) {
      bool use_graal_jit = match_option(option, "-XX:+UseGraalJIT");
      if (use_graal_jit) {
        const char* jvmci_compiler = get_property("jvmci.Compiler");
        if (jvmci_compiler != nullptr) {
          if (strncmp(jvmci_compiler, "graal", strlen("graal")) != 0) {
            jio_fprintf(defaultStream::error_stream(),
              "Value of jvmci.Compiler incompatible with +UseGraalJIT: %s\n", jvmci_compiler);
            return JNI_ERR;
          }
        } else if (!add_property("jvmci.Compiler=graal")) {
            return JNI_ENOMEM;
        }
      }

      // Just continue, since "-XX:+EnableJVMCIProduct" or "-XX:+UseGraalJIT" has been specified before
      if (EnableJVMCIProduct) {
        continue;
      }
      JVMFlag *jvmciFlag = JVMFlag::find_flag("EnableJVMCIProduct");
      // Allow this flag if it has been unlocked.
      if (jvmciFlag != nullptr && jvmciFlag->is_unlocked()) {
        if (!JVMCIGlobals::enable_jvmci_product_mode(origin, use_graal_jit)) {
          jio_fprintf(defaultStream::error_stream(),
            "Unable to enable JVMCI in product mode\n");
          return JNI_ERR;
        }
      }
      // The flag was locked so process normally to report that error
      else if (!process_argument(use_graal_jit ? "UseGraalJIT" : "EnableJVMCIProduct", args->ignoreUnrecognized, origin)) {
        return JNI_EINVAL;
      }
#endif // INCLUDE_JVMCI
#if INCLUDE_JFR
    } else if (match_jfr_option(&option)) {
      return JNI_EINVAL;
#endif
    } else if (match_option(option, "-XX:", &tail)) { // -XX:xxxx
      // Skip -XX:Flags= and -XX:VMOptionsFile= since those cases have
      // already been handled
      if ((strncmp(tail, "Flags=", strlen("Flags=")) != 0) &&
          (strncmp(tail, "VMOptionsFile=", strlen("VMOptionsFile=")) != 0)) {
        if (!process_argument(tail, args->ignoreUnrecognized, origin)) {
          return JNI_EINVAL;
        }
      }
    // Unknown option
    } else if (is_bad_option(option, args->ignoreUnrecognized)) {
      return JNI_ERR;
    }
  }

  // PrintSharedArchiveAndExit will turn on
  //   -Xshare:on
  //   -Xlog:class+path=info
  if (PrintSharedArchiveAndExit) {
    UseSharedSpaces = true;
    RequireSharedSpaces = true;
    LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(class, path));
  }

  fix_appclasspath();

  return JNI_OK;
}

void Arguments::add_patch_mod_prefix(const char* module_name, const char* path) {
  // For java.base check for duplicate --patch-module options being specified on the command line.
  // This check is only required for java.base, all other duplicate module specifications
  // will be checked during module system initialization.  The module system initialization
  // will throw an ExceptionInInitializerError if this situation occurs.
  if (strcmp(module_name, JAVA_BASE_NAME) == 0) {
    if (patch_mod_javabase) {
      vm_exit_during_initialization("Cannot specify " JAVA_BASE_NAME " more than once to --patch-module");
    } else {
      patch_mod_javabase = true;
    }
  }

  // Create GrowableArray lazily, only if --patch-module has been specified
  if (_patch_mod_prefix == nullptr) {
    _patch_mod_prefix = new (mtArguments) GrowableArray<ModulePatchPath*>(10, mtArguments);
  }

  _patch_mod_prefix->push(new ModulePatchPath(module_name, path));
}

// Remove all empty paths from the app classpath (if IgnoreEmptyClassPaths is enabled)
//
// This is necessary because some apps like to specify classpath like -cp foo.jar:${XYZ}:bar.jar
// in their start-up scripts. If XYZ is empty, the classpath will look like "-cp foo.jar::bar.jar".
// Java treats such empty paths as if the user specified "-cp foo.jar:.:bar.jar". I.e., an empty
// path is treated as the current directory.
//
// This causes problems with CDS, which requires that all directories specified in the classpath
// must be empty. In most cases, applications do NOT want to load classes from the current
// directory anyway. Adding -XX:+IgnoreEmptyClassPaths will make these applications' start-up
// scripts compatible with CDS.
void Arguments::fix_appclasspath() {
  if (IgnoreEmptyClassPaths) {
    const char separator = *os::path_separator();
    const char* src = _java_class_path->value();

    // skip over all the leading empty paths
    while (*src == separator) {
      src ++;
    }

    char* copy = os::strdup_check_oom(src, mtArguments);

    // trim all trailing empty paths
    for (char* tail = copy + strlen(copy) - 1; tail >= copy && *tail == separator; tail--) {
      *tail = '\0';
    }

    char from[3] = {separator, separator, '\0'};
    char to  [2] = {separator, '\0'};
    while (StringUtils::replace_no_expand(copy, from, to) > 0) {
      // Keep replacing "::" -> ":" until we have no more "::" (non-windows)
      // Keep replacing ";;" -> ";" until we have no more ";;" (windows)
    }

    _java_class_path->set_writeable_value(copy);
    FreeHeap(copy); // a copy was made by set_value, so don't need this anymore
  }
}

jint Arguments::finalize_vm_init_args() {
  // check if the default lib/endorsed directory exists; if so, error
  char path[JVM_MAXPATHLEN];
  const char* fileSep = os::file_separator();
  jio_snprintf(path, JVM_MAXPATHLEN, "%s%slib%sendorsed", Arguments::get_java_home(), fileSep, fileSep);

  DIR* dir = os::opendir(path);
  if (dir != nullptr) {
    jio_fprintf(defaultStream::output_stream(),
      "<JAVA_HOME>/lib/endorsed is not supported. Endorsed standards and standalone APIs\n"
      "in modular form will be supported via the concept of upgradeable modules.\n");
    os::closedir(dir);
    return JNI_ERR;
  }

  jio_snprintf(path, JVM_MAXPATHLEN, "%s%slib%sext", Arguments::get_java_home(), fileSep, fileSep);
  dir = os::opendir(path);
  if (dir != nullptr) {
    jio_fprintf(defaultStream::output_stream(),
      "<JAVA_HOME>/lib/ext exists, extensions mechanism no longer supported; "
      "Use -classpath instead.\n.");
    os::closedir(dir);
    return JNI_ERR;
  }

  // This must be done after all arguments have been processed
  // and the container support has been initialized since AggressiveHeap
  // relies on the amount of total memory available.
  if (AggressiveHeap) {
    jint result = set_aggressive_heap_flags();
    if (result != JNI_OK) {
      return result;
    }
  }

  // CompileThresholdScaling == 0.0 is same as -Xint: Disable compilation (enable interpreter-only mode),
  // but like -Xint, leave compilation thresholds unaffected.
  // With tiered compilation disabled, setting CompileThreshold to 0 disables compilation as well.
  if ((CompileThresholdScaling == 0.0) || (!TieredCompilation && CompileThreshold == 0)) {
    set_mode_flags(_int);
  }

#ifdef ZERO
  // Zero always runs in interpreted mode
  set_mode_flags(_int);
#endif

  // eventually fix up InitialTenuringThreshold if only MaxTenuringThreshold is set
  if (FLAG_IS_DEFAULT(InitialTenuringThreshold) && (InitialTenuringThreshold > MaxTenuringThreshold)) {
    FLAG_SET_ERGO(InitialTenuringThreshold, MaxTenuringThreshold);
  }

#if !COMPILER2_OR_JVMCI
  // Don't degrade server performance for footprint
  if (FLAG_IS_DEFAULT(UseLargePages) &&
      MaxHeapSize < LargePageHeapSizeThreshold) {
    // No need for large granularity pages w/small heaps.
    // Note that large pages are enabled/disabled for both the
    // Java heap and the code cache.
    FLAG_SET_DEFAULT(UseLargePages, false);
  }

  UNSUPPORTED_OPTION(ProfileInterpreter);
#endif

  // Parse the CompilationMode flag
  if (!CompilationModeFlag::initialize()) {
    return JNI_ERR;
  }

  if (!check_vm_args_consistency()) {
    return JNI_ERR;
  }


#ifndef CAN_SHOW_REGISTERS_ON_ASSERT
  UNSUPPORTED_OPTION(ShowRegistersOnAssert);
#endif // CAN_SHOW_REGISTERS_ON_ASSERT

  return JNI_OK;
}

// Helper class for controlling the lifetime of JavaVMInitArgs
// objects.  The contents of the JavaVMInitArgs are guaranteed to be
// deleted on the destruction of the ScopedVMInitArgs object.
class ScopedVMInitArgs : public StackObj {
 private:
  JavaVMInitArgs _args;
  char*          _container_name;
  bool           _is_set;
  char*          _vm_options_file_arg;

 public:
  ScopedVMInitArgs(const char *container_name) {
    _args.version = JNI_VERSION_1_2;
    _args.nOptions = 0;
    _args.options = nullptr;
    _args.ignoreUnrecognized = false;
    _container_name = (char *)container_name;
    _is_set = false;
    _vm_options_file_arg = nullptr;
  }

  // Populates the JavaVMInitArgs object represented by this
  // ScopedVMInitArgs object with the arguments in options.  The
  // allocated memory is deleted by the destructor.  If this method
  // returns anything other than JNI_OK, then this object is in a
  // partially constructed state, and should be abandoned.
  jint set_args(const GrowableArrayView<JavaVMOption>* options) {
    _is_set = true;
    JavaVMOption* options_arr = NEW_C_HEAP_ARRAY_RETURN_NULL(
        JavaVMOption, options->length(), mtArguments);
    if (options_arr == nullptr) {
      return JNI_ENOMEM;
    }
    _args.options = options_arr;

    for (int i = 0; i < options->length(); i++) {
      options_arr[i] = options->at(i);
      options_arr[i].optionString = os::strdup(options_arr[i].optionString);
      if (options_arr[i].optionString == nullptr) {
        // Rely on the destructor to do cleanup.
        _args.nOptions = i;
        return JNI_ENOMEM;
      }
    }

    _args.nOptions = options->length();
    _args.ignoreUnrecognized = IgnoreUnrecognizedVMOptions;
    return JNI_OK;
  }

  JavaVMInitArgs* get()             { return &_args; }
  char* container_name()            { return _container_name; }
  bool  is_set()                    { return _is_set; }
  bool  found_vm_options_file_arg() { return _vm_options_file_arg != nullptr; }
  char* vm_options_file_arg()       { return _vm_options_file_arg; }

  void set_vm_options_file_arg(const char *vm_options_file_arg) {
    if (_vm_options_file_arg != nullptr) {
      os::free(_vm_options_file_arg);
    }
    _vm_options_file_arg = os::strdup_check_oom(vm_options_file_arg);
  }

  ~ScopedVMInitArgs() {
    if (_vm_options_file_arg != nullptr) {
      os::free(_vm_options_file_arg);
    }
    if (_args.options == nullptr) return;
    for (int i = 0; i < _args.nOptions; i++) {
      os::free(_args.options[i].optionString);
    }
    FREE_C_HEAP_ARRAY(JavaVMOption, _args.options);
  }

  // Insert options into this option list, to replace option at
  // vm_options_file_pos (-XX:VMOptionsFile)
  jint insert(const JavaVMInitArgs* args,
              const JavaVMInitArgs* args_to_insert,
              const int vm_options_file_pos) {
    assert(_args.options == nullptr, "shouldn't be set yet");
    assert(args_to_insert->nOptions != 0, "there should be args to insert");
    assert(vm_options_file_pos != -1, "vm_options_file_pos should be set");

    int length = args->nOptions + args_to_insert->nOptions - 1;
    // Construct new option array
    GrowableArrayCHeap<JavaVMOption, mtArguments> options(length);
    for (int i = 0; i < args->nOptions; i++) {
      if (i == vm_options_file_pos) {
        // insert the new options starting at the same place as the
        // -XX:VMOptionsFile option
        for (int j = 0; j < args_to_insert->nOptions; j++) {
          options.push(args_to_insert->options[j]);
        }
      } else {
        options.push(args->options[i]);
      }
    }
    // make into options array
    return set_args(&options);
  }
};

jint Arguments::parse_java_options_environment_variable(ScopedVMInitArgs* args) {
  return parse_options_environment_variable("_JAVA_OPTIONS", args);
}

jint Arguments::parse_java_tool_options_environment_variable(ScopedVMInitArgs* args) {
  return parse_options_environment_variable("JAVA_TOOL_OPTIONS", args);
}

static JavaVMOption* get_last_aotmode_arg(const JavaVMInitArgs* args) {
  for (int index = args->nOptions - 1; index >= 0; index--) {
    JavaVMOption* option = args->options + index;
    if (strstr(option->optionString, "-XX:AOTMode=") == option->optionString) {
      return option;
    }
  }

  return nullptr;
}

jint Arguments::parse_jdk_aot_vm_options_environment_variable(GrowableArrayCHeap<VMInitArgsGroup, mtArguments>* all_args,
                                                            ScopedVMInitArgs* jdk_aot_vm_options_args) {
  // Don't bother scanning all the args if this env variable is not set
  if (::getenv("JDK_AOT_VM_OPTIONS") == nullptr) {
    return JNI_OK;
  }

  // Scan backwards and find the last occurrence of -XX:AOTMode=xxx, which will decide the value
  // of AOTMode.
  JavaVMOption* option = nullptr;
  for (int i = all_args->length() - 1; i >= 0; i--) {
    if ((option = get_last_aotmode_arg(all_args->at(i)._args)) != nullptr) {
      break;
    }
  }

  if (option != nullptr) {
    // We have found the last -XX:AOTMode=xxx. At this point <option> has NOT been parsed yet,
    // so its value is not reflected inside the global variable AOTMode.
    if (strcmp(option->optionString, "-XX:AOTMode=create") != 0) {
      return JNI_OK; // Do not parse JDK_AOT_VM_OPTIONS
    }
  } else {
    // -XX:AOTMode is not specified in any of 4 options_args, let's check AOTMode,
    // which would have been set inside process_settings_file();
    if (AOTMode == nullptr || strcmp(AOTMode, "create") != 0) {
      return JNI_OK; // Do not parse JDK_AOT_VM_OPTIONS
    }
  }

  return parse_options_environment_variable("JDK_AOT_VM_OPTIONS", jdk_aot_vm_options_args);
}

jint Arguments::parse_options_environment_variable(const char* name,
                                                   ScopedVMInitArgs* vm_args) {
  char *buffer = ::getenv(name);

  // Don't check this environment variable if user has special privileges
  // (e.g. unix su command).
  if (buffer == nullptr || os::have_special_privileges()) {
    return JNI_OK;
  }

  if ((buffer = os::strdup(buffer)) == nullptr) {
    return JNI_ENOMEM;
  }

  jio_fprintf(defaultStream::error_stream(),
              "Picked up %s: %s\n", name, buffer);

  int retcode = parse_options_buffer(name, buffer, strlen(buffer), vm_args);

  os::free(buffer);
  return retcode;
}

jint Arguments::parse_vm_options_file(const char* file_name, ScopedVMInitArgs* vm_args) {
  // read file into buffer
  int fd = ::open(file_name, O_RDONLY);
  if (fd < 0) {
    jio_fprintf(defaultStream::error_stream(),
                "Could not open options file '%s'\n",
                file_name);
    return JNI_ERR;
  }

  struct stat stbuf;
  int retcode = os::stat(file_name, &stbuf);
  if (retcode != 0) {
    jio_fprintf(defaultStream::error_stream(),
                "Could not stat options file '%s'\n",
                file_name);
    ::close(fd);
    return JNI_ERR;
  }

  if (stbuf.st_size == 0) {
    // tell caller there is no option data and that is ok
    ::close(fd);
    return JNI_OK;
  }

  // '+ 1' for null termination even with max bytes
  size_t bytes_alloc = stbuf.st_size + 1;

  char *buf = NEW_C_HEAP_ARRAY_RETURN_NULL(char, bytes_alloc, mtArguments);
  if (nullptr == buf) {
    jio_fprintf(defaultStream::error_stream(),
                "Could not allocate read buffer for options file parse\n");
    ::close(fd);
    return JNI_ENOMEM;
  }

  memset(buf, 0, bytes_alloc);

  // Fill buffer
  ssize_t bytes_read = ::read(fd, (void *)buf, (unsigned)bytes_alloc);
  ::close(fd);
  if (bytes_read < 0) {
    FREE_C_HEAP_ARRAY(char, buf);
    jio_fprintf(defaultStream::error_stream(),
                "Could not read options file '%s'\n", file_name);
    return JNI_ERR;
  }

  if (bytes_read == 0) {
    // tell caller there is no option data and that is ok
    FREE_C_HEAP_ARRAY(char, buf);
    return JNI_OK;
  }

  retcode = parse_options_buffer(file_name, buf, bytes_read, vm_args);

  FREE_C_HEAP_ARRAY(char, buf);
  return retcode;
}

jint Arguments::parse_options_buffer(const char* name, char* buffer, const size_t buf_len, ScopedVMInitArgs* vm_args) {
  // Construct option array
  GrowableArrayCHeap<JavaVMOption, mtArguments> options(2);

  // some pointers to help with parsing
  char *buffer_end = buffer + buf_len;
  char *opt_hd = buffer;
  char *wrt = buffer;
  char *rd = buffer;

  // parse all options
  while (rd < buffer_end) {
    // skip leading white space from the input string
    while (rd < buffer_end && isspace((unsigned char) *rd)) {
      rd++;
    }

    if (rd >= buffer_end) {
      break;
    }

    // Remember this is where we found the head of the token.
    opt_hd = wrt;

    // Tokens are strings of non white space characters separated
    // by one or more white spaces.
    while (rd < buffer_end && !isspace((unsigned char) *rd)) {
      if (*rd == '\'' || *rd == '"') {      // handle a quoted string
        int quote = *rd;                    // matching quote to look for
        rd++;                               // don't copy open quote
        while (rd < buffer_end && *rd != quote) {
                                            // include everything (even spaces)
                                            // up until the close quote
          *wrt++ = *rd++;                   // copy to option string
        }

        if (rd < buffer_end) {
          rd++;                             // don't copy close quote
        } else {
                                            // did not see closing quote
          jio_fprintf(defaultStream::error_stream(),
                      "Unmatched quote in %s\n", name);
          return JNI_ERR;
        }
      } else {
        *wrt++ = *rd++;                     // copy to option string
      }
    }

    // steal a white space character and set it to null
    *wrt++ = '\0';
    // We now have a complete token

    JavaVMOption option;
    option.optionString = opt_hd;
    option.extraInfo = nullptr;

    options.append(option);                // Fill in option

    rd++;  // Advance to next character
  }

  // Fill out JavaVMInitArgs structure.
  return vm_args->set_args(&options);
}

#ifndef PRODUCT
// Determine whether LogVMOutput should be implicitly turned on.
static bool use_vm_log() {
  if (LogCompilation || !FLAG_IS_DEFAULT(LogFile) ||
      PrintCompilation || PrintInlining || PrintDependencies || PrintNativeNMethods ||
      PrintDebugInfo || PrintRelocations || PrintNMethods || PrintExceptionHandlers ||
      PrintAssembly || TraceDeoptimization ||
      (VerifyDependencies && FLAG_IS_CMDLINE(VerifyDependencies))) {
    return true;
  }

#ifdef COMPILER1
  if (PrintC1Statistics) {
    return true;
  }
#endif // COMPILER1

#ifdef COMPILER2
  if (PrintOptoAssembly || PrintOptoStatistics) {
    return true;
  }
#endif // COMPILER2

  return false;
}

#endif // PRODUCT

bool Arguments::args_contains_vm_options_file_arg(const JavaVMInitArgs* args) {
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    const char* tail;
    if (match_option(option, "-XX:VMOptionsFile=", &tail)) {
      return true;
    }
  }
  return false;
}

jint Arguments::insert_vm_options_file(const JavaVMInitArgs* args,
                                       const char* vm_options_file,
                                       const int vm_options_file_pos,
                                       ScopedVMInitArgs* vm_options_file_args,
                                       ScopedVMInitArgs* args_out) {
  jint code = parse_vm_options_file(vm_options_file, vm_options_file_args);
  if (code != JNI_OK) {
    return code;
  }

  if (vm_options_file_args->get()->nOptions < 1) {
    return JNI_OK;
  }

  if (args_contains_vm_options_file_arg(vm_options_file_args->get())) {
    jio_fprintf(defaultStream::error_stream(),
                "A VM options file may not refer to a VM options file. "
                "Specification of '-XX:VMOptionsFile=<file-name>' in the "
                "options file '%s' in options container '%s' is an error.\n",
                vm_options_file_args->vm_options_file_arg(),
                vm_options_file_args->container_name());
    return JNI_EINVAL;
  }

  return args_out->insert(args, vm_options_file_args->get(),
                          vm_options_file_pos);
}

// Expand -XX:VMOptionsFile found in args_in as needed.
// mod_args and args_out parameters may return values as needed.
jint Arguments::expand_vm_options_as_needed(const JavaVMInitArgs* args_in,
                                            ScopedVMInitArgs* mod_args,
                                            JavaVMInitArgs** args_out) {
  jint code = match_special_option_and_act(args_in, mod_args);
  if (code != JNI_OK) {
    return code;
  }

  if (mod_args->is_set()) {
    // args_in contains -XX:VMOptionsFile and mod_args contains the
    // original options from args_in along with the options expanded
    // from the VMOptionsFile. Return a short-hand to the caller.
    *args_out = mod_args->get();
  } else {
    *args_out = (JavaVMInitArgs *)args_in;  // no changes so use args_in
  }
  return JNI_OK;
}

jint Arguments::match_special_option_and_act(const JavaVMInitArgs* args,
                                             ScopedVMInitArgs* args_out) {
  // Remaining part of option string
  const char* tail;
  ScopedVMInitArgs vm_options_file_args(args_out->container_name());

  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    if (match_option(option, "-XX:Flags=", &tail)) {
      Arguments::set_jvm_flags_file(tail);
      continue;
    }
    if (match_option(option, "-XX:VMOptionsFile=", &tail)) {
      if (vm_options_file_args.found_vm_options_file_arg()) {
        jio_fprintf(defaultStream::error_stream(),
                    "The option '%s' is already specified in the options "
                    "container '%s' so the specification of '%s' in the "
                    "same options container is an error.\n",
                    vm_options_file_args.vm_options_file_arg(),
                    vm_options_file_args.container_name(),
                    option->optionString);
        return JNI_EINVAL;
      }
      vm_options_file_args.set_vm_options_file_arg(option->optionString);
      // If there's a VMOptionsFile, parse that
      jint code = insert_vm_options_file(args, tail, index,
                                         &vm_options_file_args, args_out);
      if (code != JNI_OK) {
        return code;
      }
      args_out->set_vm_options_file_arg(vm_options_file_args.vm_options_file_arg());
      if (args_out->is_set()) {
        // The VMOptions file inserted some options so switch 'args'
        // to the new set of options, and continue processing which
        // preserves "last option wins" semantics.
        args = args_out->get();
        // The first option from the VMOptionsFile replaces the
        // current option.  So we back track to process the
        // replacement option.
        index--;
      }
      continue;
    }
    if (match_option(option, "-XX:+PrintVMOptions")) {
      PrintVMOptions = true;
      continue;
    }
    if (match_option(option, "-XX:-PrintVMOptions")) {
      PrintVMOptions = false;
      continue;
    }
    if (match_option(option, "-XX:+IgnoreUnrecognizedVMOptions")) {
      IgnoreUnrecognizedVMOptions = true;
      continue;
    }
    if (match_option(option, "-XX:-IgnoreUnrecognizedVMOptions")) {
      IgnoreUnrecognizedVMOptions = false;
      continue;
    }
    if (match_option(option, "-XX:+PrintFlagsInitial")) {
      JVMFlag::printFlags(tty, false);
      vm_exit(0);
    }

#ifndef PRODUCT
    if (match_option(option, "-XX:+PrintFlagsWithComments")) {
      JVMFlag::printFlags(tty, true);
      vm_exit(0);
    }
#endif
  }
  return JNI_OK;
}

static void print_options(const JavaVMInitArgs *args) {
  const char* tail;
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption *option = args->options + index;
    if (match_option(option, "-XX:", &tail)) {
      logOption(tail);
    }
  }
}

bool Arguments::handle_deprecated_print_gc_flags() {
  if (PrintGC) {
    log_warning(gc)("-XX:+PrintGC is deprecated. Will use -Xlog:gc instead.");
  }
  if (PrintGCDetails) {
    log_warning(gc)("-XX:+PrintGCDetails is deprecated. Will use -Xlog:gc* instead.");
  }

  if (_legacyGCLogging.lastFlag == 2) {
    // -Xloggc was used to specify a filename
    const char* gc_conf = PrintGCDetails ? "gc*" : "gc";

    LogTarget(Error, logging) target;
    LogStream errstream(target);
    return LogConfiguration::parse_log_arguments(_legacyGCLogging.file, gc_conf, nullptr, nullptr, &errstream);
  } else if (PrintGC || PrintGCDetails || (_legacyGCLogging.lastFlag == 1)) {
    LogConfiguration::configure_stdout(LogLevel::Info, !PrintGCDetails, LOG_TAGS(gc));
  }
  return true;
}

static void apply_debugger_ergo() {
#ifdef ASSERT
  if (ReplayCompiles) {
    FLAG_SET_ERGO_IF_DEFAULT(UseDebuggerErgo, true);
  }

  if (UseDebuggerErgo) {
    // Turn on sub-flags
    FLAG_SET_ERGO_IF_DEFAULT(UseDebuggerErgo1, true);
    FLAG_SET_ERGO_IF_DEFAULT(UseDebuggerErgo2, true);
  }

  if (UseDebuggerErgo2) {
    // Debugging with limited number of CPUs
    FLAG_SET_ERGO_IF_DEFAULT(UseNUMA, false);
    FLAG_SET_ERGO_IF_DEFAULT(ConcGCThreads, 1);
    FLAG_SET_ERGO_IF_DEFAULT(ParallelGCThreads, 1);
    FLAG_SET_ERGO_IF_DEFAULT(CICompilerCount, 2);
  }
#endif // ASSERT
}

// Parse entry point called from JNI_CreateJavaVM

jint Arguments::parse(const JavaVMInitArgs* initial_cmd_args) {
  assert(verify_special_jvm_flags(false), "deprecated and obsolete flag table inconsistent");
  JVMFlag::check_all_flag_declarations();

  // If flag "-XX:Flags=flags-file" is used it will be the first option to be processed.
  const char* hotspotrc = ".hotspotrc";
  bool settings_file_specified = false;
  bool needs_hotspotrc_warning = false;
  ScopedVMInitArgs initial_vm_options_args("");
  ScopedVMInitArgs initial_java_tool_options_args("env_var='JAVA_TOOL_OPTIONS'");
  ScopedVMInitArgs initial_java_options_args("env_var='_JAVA_OPTIONS'");
  ScopedVMInitArgs initial_jdk_aot_vm_options_args("env_var='JDK_AOT_VM_OPTIONS'");

  // Pointers to current working set of containers
  JavaVMInitArgs* cur_cmd_args;
  JavaVMInitArgs* cur_vm_options_args;
  JavaVMInitArgs* cur_java_options_args;
  JavaVMInitArgs* cur_java_tool_options_args;
  JavaVMInitArgs* cur_jdk_aot_vm_options_args;

  // Containers for modified/expanded options
  ScopedVMInitArgs mod_cmd_args("cmd_line_args");
  ScopedVMInitArgs mod_vm_options_args("vm_options_args");
  ScopedVMInitArgs mod_java_tool_options_args("env_var='JAVA_TOOL_OPTIONS'");
  ScopedVMInitArgs mod_java_options_args("env_var='_JAVA_OPTIONS'");
  ScopedVMInitArgs mod_jdk_aot_vm_options_args("env_var='_JDK_AOT_VM_OPTIONS'");

  GrowableArrayCHeap<VMInitArgsGroup, mtArguments> all_args;

  jint code =
      parse_java_tool_options_environment_variable(&initial_java_tool_options_args);
  if (code != JNI_OK) {
    return code;
  }

  // Yet another environment variable: _JAVA_OPTIONS. This mimics the classic VM.
  // This is an undocumented feature.
  code = parse_java_options_environment_variable(&initial_java_options_args);
  if (code != JNI_OK) {
    return code;
  }

  // Parse the options in the /java.base/jdk/internal/vm/options resource, if present
  char *vmoptions = ClassLoader::lookup_vm_options();
  if (vmoptions != nullptr) {
    code = parse_options_buffer("vm options resource", vmoptions, strlen(vmoptions), &initial_vm_options_args);
    FREE_C_HEAP_ARRAY(char, vmoptions);
    if (code != JNI_OK) {
      return code;
    }
  }

  code = expand_vm_options_as_needed(initial_java_tool_options_args.get(),
                                     &mod_java_tool_options_args,
                                     &cur_java_tool_options_args);
  if (code != JNI_OK) {
    return code;
  }

  code = expand_vm_options_as_needed(initial_cmd_args,
                                     &mod_cmd_args,
                                     &cur_cmd_args);
  if (code != JNI_OK) {
    return code;
  }

  code = expand_vm_options_as_needed(initial_java_options_args.get(),
                                     &mod_java_options_args,
                                     &cur_java_options_args);
  if (code != JNI_OK) {
    return code;
  }

  code = expand_vm_options_as_needed(initial_vm_options_args.get(),
                                     &mod_vm_options_args,
                                     &cur_vm_options_args);
  if (code != JNI_OK) {
    return code;
  }

  const char* flags_file = Arguments::get_jvm_flags_file();
  settings_file_specified = (flags_file != nullptr);

  // Parse specified settings file (s) -- the effects are applied immediately into the JVM global flags.
  if (settings_file_specified) {
    if (!process_settings_file(flags_file, true,
                               IgnoreUnrecognizedVMOptions)) {
      return JNI_EINVAL;
    }
  } else {
#ifdef ASSERT
    // Parse default .hotspotrc settings file
    if (!process_settings_file(".hotspotrc", false,
                               IgnoreUnrecognizedVMOptions)) {
      return JNI_EINVAL;
    }
#else
    struct stat buf;
    if (os::stat(hotspotrc, &buf) == 0) {
      needs_hotspotrc_warning = true;
    }
#endif
  }

  // The settings in the args are applied in this order to the the JVM global flags.
  // For historical reasons, the order is DIFFERENT than the scanning order of
  // the above expand_vm_options_as_needed() calls.
  all_args.append({cur_vm_options_args, JVMFlagOrigin::JIMAGE_RESOURCE});
  all_args.append({cur_java_tool_options_args, JVMFlagOrigin::ENVIRON_VAR});
  all_args.append({cur_cmd_args, JVMFlagOrigin::COMMAND_LINE});
  all_args.append({cur_java_options_args, JVMFlagOrigin::ENVIRON_VAR});

  // JDK_AOT_VM_OPTIONS are parsed only if -XX:AOTMode=create has been detected from all
  // the options that have been gathered above.
  code = parse_jdk_aot_vm_options_environment_variable(&all_args, &initial_jdk_aot_vm_options_args);
  if (code != JNI_OK) {
    return code;
  }
  code = expand_vm_options_as_needed(initial_jdk_aot_vm_options_args.get(),
                                     &mod_jdk_aot_vm_options_args,
                                     &cur_jdk_aot_vm_options_args);
  if (code != JNI_OK) {
    return code;
  }

  for (int index = 0; index < cur_jdk_aot_vm_options_args->nOptions; index++) {
    JavaVMOption* option = cur_jdk_aot_vm_options_args->options + index;
    const char* optionString = option->optionString;
    if (strstr(optionString, "-XX:AOTMode=") == optionString &&
        strcmp(optionString, "-XX:AOTMode=create") != 0) {
      jio_fprintf(defaultStream::error_stream(),
                  "Option %s cannot be specified in JDK_AOT_VM_OPTIONS\n", optionString);
      return JNI_ERR;
    }
  }

  all_args.append({cur_jdk_aot_vm_options_args, JVMFlagOrigin::ENVIRON_VAR});

  if (IgnoreUnrecognizedVMOptions) {
    // Note: unrecognized options in cur_vm_options_arg cannot be ignored. They are part of
    // the JDK so it shouldn't have bad options.
    cur_cmd_args->ignoreUnrecognized = true;
    cur_java_tool_options_args->ignoreUnrecognized = true;
    cur_java_options_args->ignoreUnrecognized = true;
    cur_jdk_aot_vm_options_args->ignoreUnrecognized = true;
  }

  if (PrintVMOptions) {
    // For historical reasons, options specified in cur_vm_options_arg and -XX:Flags are not printed.
    print_options(cur_java_tool_options_args);
    print_options(cur_cmd_args);
    print_options(cur_java_options_args);
    print_options(cur_jdk_aot_vm_options_args);
  }

  // Apply the settings in these args to the JVM global flags.
  jint result = parse_vm_init_args(&all_args);

  if (result != JNI_OK) {
    return result;
  }

  // Delay warning until here so that we've had a chance to process
  // the -XX:-PrintWarnings flag
  if (needs_hotspotrc_warning) {
    warning("%s file is present but has been ignored.  "
            "Run with -XX:Flags=%s to load the file.",
            hotspotrc, hotspotrc);
  }

  if (needs_module_property_warning) {
    warning("Ignoring system property options whose names match the '-Djdk.module.*'."
            " names that are reserved for internal use.");
  }

#if defined(_ALLBSD_SOURCE) || defined(AIX)  // UseLargePages is not yet supported on BSD and AIX.
  UNSUPPORTED_OPTION(UseLargePages);
#endif

#if defined(AIX)
  UNSUPPORTED_OPTION_NULL(AllocateHeapAt);
#endif

#ifndef PRODUCT
  if (TraceBytecodesAt != 0) {
    TraceBytecodes = true;
  }
#endif // PRODUCT

  if (ScavengeRootsInCode == 0) {
    if (!FLAG_IS_DEFAULT(ScavengeRootsInCode)) {
      warning("Forcing ScavengeRootsInCode non-zero");
    }
    ScavengeRootsInCode = 1;
  }

  if (!handle_deprecated_print_gc_flags()) {
    return JNI_EINVAL;
  }

  // Set object alignment values.
  set_object_alignment();

#if !INCLUDE_CDS
  if (CDSConfig::is_dumping_static_archive() || RequireSharedSpaces) {
    jio_fprintf(defaultStream::error_stream(),
      "Shared spaces are not supported in this VM\n");
    return JNI_ERR;
  }
  if (DumpLoadedClassList != nullptr) {
    jio_fprintf(defaultStream::error_stream(),
      "DumpLoadedClassList is not supported in this VM\n");
    return JNI_ERR;
  }
  if ((CDSConfig::is_using_archive() && xshare_auto_cmd_line) ||
      log_is_enabled(Info, cds) || log_is_enabled(Info, aot)) {
    warning("Shared spaces are not supported in this VM");
    UseSharedSpaces = false;
    LogConfiguration::configure_stdout(LogLevel::Off, true, LOG_TAGS(cds));
    LogConfiguration::configure_stdout(LogLevel::Off, true, LOG_TAGS(aot));
  }
  no_shared_spaces("CDS Disabled");
#endif // INCLUDE_CDS

  // Verify NMT arguments
  const NMT_TrackingLevel lvl = NMTUtil::parse_tracking_level(NativeMemoryTracking);
  if (lvl == NMT_unknown) {
    jio_fprintf(defaultStream::error_stream(),
                "Syntax error, expecting -XX:NativeMemoryTracking=[off|summary|detail]\n");
    return JNI_ERR;
  }
  if (PrintNMTStatistics && lvl == NMT_off) {
    warning("PrintNMTStatistics is disabled, because native memory tracking is not enabled");
    FLAG_SET_DEFAULT(PrintNMTStatistics, false);
  }

  bool trace_dependencies = log_is_enabled(Debug, dependencies);
  if (trace_dependencies && VerifyDependencies) {
    warning("dependency logging results may be inflated by VerifyDependencies");
  }

  bool log_class_load_cause = log_is_enabled(Info, class, load, cause, native) ||
                              log_is_enabled(Info, class, load, cause);
  if (log_class_load_cause && LogClassLoadingCauseFor == nullptr) {
    warning("class load cause logging will not produce output without LogClassLoadingCauseFor");
  }

  apply_debugger_ergo();

  // The VMThread needs to stop now and then to execute these debug options.
  if ((HandshakeALot || SafepointALot) && FLAG_IS_DEFAULT(GuaranteedSafepointInterval)) {
    FLAG_SET_DEFAULT(GuaranteedSafepointInterval, 1000);
  }

  if (log_is_enabled(Info, arguments)) {
    LogStream st(Log(arguments)::info());
    Arguments::print_on(&st);
  }

  return JNI_OK;
}

void Arguments::set_compact_headers_flags() {
#ifdef _LP64
  if (UseCompactObjectHeaders && FLAG_IS_CMDLINE(UseCompressedClassPointers) && !UseCompressedClassPointers) {
    warning("Compact object headers require compressed class pointers. Disabling compact object headers.");
    FLAG_SET_DEFAULT(UseCompactObjectHeaders, false);
  }
  if (UseCompactObjectHeaders && !UseObjectMonitorTable) {
    // If UseCompactObjectHeaders is on the command line, turn on UseObjectMonitorTable.
    if (FLAG_IS_CMDLINE(UseCompactObjectHeaders)) {
      FLAG_SET_DEFAULT(UseObjectMonitorTable, true);

      // If UseObjectMonitorTable is on the command line, turn off UseCompactObjectHeaders.
    } else if (FLAG_IS_CMDLINE(UseObjectMonitorTable)) {
      FLAG_SET_DEFAULT(UseCompactObjectHeaders, false);
      // If neither on the command line, the defaults are incompatible, but turn on UseObjectMonitorTable.
    } else {
      FLAG_SET_DEFAULT(UseObjectMonitorTable, true);
    }
  }
  if (UseCompactObjectHeaders && !UseCompressedClassPointers) {
    FLAG_SET_DEFAULT(UseCompressedClassPointers, true);
  }
#endif
}

jint Arguments::apply_ergo() {
  // Set flags based on ergonomics.
  jint result = set_ergonomics_flags();
  if (result != JNI_OK) return result;

  // Set heap size based on available physical memory
  set_heap_size();

  GCConfig::arguments()->initialize();

  set_compact_headers_flags();

  if (UseCompressedClassPointers) {
    CompressedKlassPointers::pre_initialize();
  }

  CDSConfig::ergo_initialize();

  // Initialize Metaspace flags and alignments
  Metaspace::ergo_initialize();

  if (!StringDedup::ergo_initialize()) {
    return JNI_EINVAL;
  }

  // Set compiler flags after GC is selected and GC specific
  // flags (LoopStripMiningIter) are set.
  CompilerConfig::ergo_initialize();

  // Set bytecode rewriting flags
  set_bytecode_flags();

  // Set flags if aggressive optimization flags are enabled
  jint code = set_aggressive_opts_flags();
  if (code != JNI_OK) {
    return code;
  }

  if (FLAG_IS_DEFAULT(UseSecondarySupersTable)) {
    FLAG_SET_DEFAULT(UseSecondarySupersTable, VM_Version::supports_secondary_supers_table());
  } else if (UseSecondarySupersTable && !VM_Version::supports_secondary_supers_table()) {
    warning("UseSecondarySupersTable is not supported");
    FLAG_SET_DEFAULT(UseSecondarySupersTable, false);
  }
  if (!UseSecondarySupersTable) {
    FLAG_SET_DEFAULT(StressSecondarySupers, false);
    FLAG_SET_DEFAULT(VerifySecondarySupers, false);
  }

#ifdef ZERO
  // Clear flags not supported on zero.
  FLAG_SET_DEFAULT(ProfileInterpreter, false);
#endif // ZERO

  if (PrintAssembly && FLAG_IS_DEFAULT(DebugNonSafepoints)) {
    warning("PrintAssembly is enabled; turning on DebugNonSafepoints to gain additional output");
    DebugNonSafepoints = true;
  }

  if (FLAG_IS_CMDLINE(CompressedClassSpaceSize) && !UseCompressedClassPointers) {
    warning("Setting CompressedClassSpaceSize has no effect when compressed class pointers are not used");
  }

  // Treat the odd case where local verification is enabled but remote
  // verification is not as if both were enabled.
  if (BytecodeVerificationLocal && !BytecodeVerificationRemote) {
    log_info(verification)("Turning on remote verification because local verification is on");
    FLAG_SET_DEFAULT(BytecodeVerificationRemote, true);
  }

#ifndef PRODUCT
  if (!LogVMOutput && FLAG_IS_DEFAULT(LogVMOutput)) {
    if (use_vm_log()) {
      LogVMOutput = true;
    }
  }
#endif // PRODUCT

  if (PrintCommandLineFlags) {
    JVMFlag::printSetFlags(tty);
  }

#if COMPILER2_OR_JVMCI
  if (!FLAG_IS_DEFAULT(EnableVectorSupport) && !EnableVectorSupport) {
    if (!FLAG_IS_DEFAULT(EnableVectorReboxing) && EnableVectorReboxing) {
      warning("Disabling EnableVectorReboxing since EnableVectorSupport is turned off.");
    }
    FLAG_SET_DEFAULT(EnableVectorReboxing, false);

    if (!FLAG_IS_DEFAULT(EnableVectorAggressiveReboxing) && EnableVectorAggressiveReboxing) {
      if (!EnableVectorReboxing) {
        warning("Disabling EnableVectorAggressiveReboxing since EnableVectorReboxing is turned off.");
      } else {
        warning("Disabling EnableVectorAggressiveReboxing since EnableVectorSupport is turned off.");
      }
    }
    FLAG_SET_DEFAULT(EnableVectorAggressiveReboxing, false);
  }
#endif // COMPILER2_OR_JVMCI

#ifdef COMPILER2
  if (!FLAG_IS_DEFAULT(UseLoopPredicate) && !UseLoopPredicate && UseProfiledLoopPredicate) {
    warning("Disabling UseProfiledLoopPredicate since UseLoopPredicate is turned off.");
    FLAG_SET_ERGO(UseProfiledLoopPredicate, false);
  }
#endif // COMPILER2

  if (log_is_enabled(Info, perf, class, link)) {
    if (!UsePerfData) {
      warning("Disabling -Xlog:perf+class+link since UsePerfData is turned off.");
      LogConfiguration::disable_tags(false, LOG_TAGS(perf, class, link));
      assert(!log_is_enabled(Info, perf, class, link), "sanity");
    }
  }

  if (FLAG_IS_CMDLINE(DiagnoseSyncOnValueBasedClasses)) {
    if (DiagnoseSyncOnValueBasedClasses == ObjectSynchronizer::LOG_WARNING && !log_is_enabled(Info, valuebasedclasses)) {
      LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(valuebasedclasses));
    }
  }
  return JNI_OK;
}

jint Arguments::adjust_after_os() {
  if (UseNUMA) {
    if (UseParallelGC) {
      if (FLAG_IS_DEFAULT(MinHeapDeltaBytes)) {
         FLAG_SET_DEFAULT(MinHeapDeltaBytes, 64*M);
      }
    }
  }
  return JNI_OK;
}

int Arguments::PropertyList_count(SystemProperty* pl) {
  int count = 0;
  while(pl != nullptr) {
    count++;
    pl = pl->next();
  }
  return count;
}

// Return the number of readable properties.
int Arguments::PropertyList_readable_count(SystemProperty* pl) {
  int count = 0;
  while(pl != nullptr) {
    if (pl->readable()) {
      count++;
    }
    pl = pl->next();
  }
  return count;
}

const char* Arguments::PropertyList_get_value(SystemProperty *pl, const char* key) {
  assert(key != nullptr, "just checking");
  SystemProperty* prop;
  for (prop = pl; prop != nullptr; prop = prop->next()) {
    if (strcmp(key, prop->key()) == 0) return prop->value();
  }
  return nullptr;
}

// Return the value of the requested property provided that it is a readable property.
const char* Arguments::PropertyList_get_readable_value(SystemProperty *pl, const char* key) {
  assert(key != nullptr, "just checking");
  SystemProperty* prop;
  // Return the property value if the keys match and the property is not internal or
  // it's the special internal property "jdk.boot.class.path.append".
  for (prop = pl; prop != nullptr; prop = prop->next()) {
    if (strcmp(key, prop->key()) == 0) {
      if (!prop->internal()) {
        return prop->value();
      } else if (strcmp(key, "jdk.boot.class.path.append") == 0) {
        return prop->value();
      } else {
        // Property is internal and not jdk.boot.class.path.append so return null.
        return nullptr;
      }
    }
  }
  return nullptr;
}

void Arguments::PropertyList_add(SystemProperty** plist, SystemProperty *new_p) {
  SystemProperty* p = *plist;
  if (p == nullptr) {
    *plist = new_p;
  } else {
    while (p->next() != nullptr) {
      p = p->next();
    }
    p->set_next(new_p);
  }
}

void Arguments::PropertyList_add(SystemProperty** plist, const char* k, const char* v,
                                 bool writeable, bool internal) {
  if (plist == nullptr)
    return;

  SystemProperty* new_p = new SystemProperty(k, v, writeable, internal);
  PropertyList_add(plist, new_p);
}

void Arguments::PropertyList_add(SystemProperty *element) {
  PropertyList_add(&_system_properties, element);
}

// This add maintains unique property key in the list.
void Arguments::PropertyList_unique_add(SystemProperty** plist, const char* k, const char* v,
                                        PropertyAppendable append, PropertyWriteable writeable,
                                        PropertyInternal internal) {
  if (plist == nullptr)
    return;

  // If property key exists and is writeable, then update with new value.
  // Trying to update a non-writeable property is silently ignored.
  SystemProperty* prop;
  for (prop = *plist; prop != nullptr; prop = prop->next()) {
    if (strcmp(k, prop->key()) == 0) {
      if (append == AppendProperty) {
        prop->append_writeable_value(v);
      } else {
        prop->set_writeable_value(v);
      }
      return;
    }
  }

  PropertyList_add(plist, k, v, writeable == WriteableProperty, internal == InternalProperty);
}

// Copies src into buf, replacing "%%" with "%" and "%p" with pid
// Returns true if all of the source pointed by src has been copied over to
// the destination buffer pointed by buf. Otherwise, returns false.
// Notes:
// 1. If the length (buflen) of the destination buffer excluding the
// null terminator character is not long enough for holding the expanded
// pid characters, it also returns false instead of returning the partially
// expanded one.
// 2. The passed in "buflen" should be large enough to hold the null terminator.
bool Arguments::copy_expand_pid(const char* src, size_t srclen,
                                char* buf, size_t buflen) {
  const char* p = src;
  char* b = buf;
  const char* src_end = &src[srclen];
  char* buf_end = &buf[buflen - 1];

  while (p < src_end && b < buf_end) {
    if (*p == '%') {
      switch (*(++p)) {
      case '%':         // "%%" ==> "%"
        *b++ = *p++;
        break;
      case 'p':  {       //  "%p" ==> current process id
        // buf_end points to the character before the last character so
        // that we could write '\0' to the end of the buffer.
        size_t buf_sz = buf_end - b + 1;
        int ret = jio_snprintf(b, buf_sz, "%d", os::current_process_id());

        // if jio_snprintf fails or the buffer is not long enough to hold
        // the expanded pid, returns false.
        if (ret < 0 || ret >= (int)buf_sz) {
          return false;
        } else {
          b += ret;
          assert(*b == '\0', "fail in copy_expand_pid");
          if (p == src_end && b == buf_end + 1) {
            // reach the end of the buffer.
            return true;
          }
        }
        p++;
        break;
      }
      default :
        *b++ = '%';
      }
    } else {
      *b++ = *p++;
    }
  }
  *b = '\0';
  return (p == src_end); // return false if not all of the source was copied
}

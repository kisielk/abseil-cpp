//
// Copyright 2019 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_FLAGS_INTERNAL_FLAG_H_
#define ABSL_FLAGS_INTERNAL_FLAG_H_

#include <stdint.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

#include "absl/base/config.h"
#include "absl/base/thread_annotations.h"
#include "absl/flags/config.h"
#include "absl/flags/internal/commandlineflag.h"
#include "absl/flags/internal/registry.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace flags_internal {

template <typename T>
class Flag;

///////////////////////////////////////////////////////////////////////////////
// Persistent state of the flag data.

template <typename T>
class FlagState : public flags_internal::FlagStateInterface {
 public:
  FlagState(Flag<T>* flag, T&& cur, bool modified, bool on_command_line,
            int64_t counter)
      : flag_(flag),
        cur_value_(std::move(cur)),
        modified_(modified),
        on_command_line_(on_command_line),
        counter_(counter) {}

  ~FlagState() override = default;

 private:
  friend class Flag<T>;

  // Restores the flag to the saved state.
  void Restore() const override;

  // Flag and saved flag data.
  Flag<T>* flag_;
  T cur_value_;
  bool modified_;
  bool on_command_line_;
  int64_t counter_;
};

///////////////////////////////////////////////////////////////////////////////
// Flag help auxiliary structs.

// This is help argument for absl::Flag encapsulating the string literal pointer
// or pointer to function generating it as well as enum descriminating two
// cases.
using HelpGenFunc = std::string (*)();

union FlagHelpMsg {
  constexpr explicit FlagHelpMsg(const char* help_msg) : literal(help_msg) {}
  constexpr explicit FlagHelpMsg(HelpGenFunc help_gen) : gen_func(help_gen) {}

  const char* literal;
  HelpGenFunc gen_func;
};

enum class FlagHelpKind : uint8_t { kLiteral = 0, kGenFunc = 1 };

struct FlagHelpArg {
  FlagHelpMsg source;
  FlagHelpKind kind;
};

extern const char kStrippedFlagHelp[];

// HelpConstexprWrap is used by struct AbslFlagHelpGenFor##name generated by
// ABSL_FLAG macro. It is only used to silence the compiler in the case where
// help message expression is not constexpr and does not have type const char*.
// If help message expression is indeed constexpr const char* HelpConstexprWrap
// is just a trivial identity function.
template <typename T>
const char* HelpConstexprWrap(const T&) {
  return nullptr;
}
constexpr const char* HelpConstexprWrap(const char* p) { return p; }
constexpr const char* HelpConstexprWrap(char* p) { return p; }

// These two HelpArg overloads allows us to select at compile time one of two
// way to pass Help argument to absl::Flag. We'll be passing
// AbslFlagHelpGenFor##name as T and integer 0 as a single argument to prefer
// first overload if possible. If T::Const is evaluatable on constexpr
// context (see non template int parameter below) we'll choose first overload.
// In this case the help message expression is immediately evaluated and is used
// to construct the absl::Flag. No additionl code is generated by ABSL_FLAG.
// Otherwise SFINAE kicks in and first overload is dropped from the
// consideration, in which case the second overload will be used. The second
// overload does not attempt to evaluate the help message expression
// immediately and instead delays the evaluation by returing the function
// pointer (&T::NonConst) genering the help message when necessary. This is
// evaluatable in constexpr context, but the cost is an extra function being
// generated in the ABSL_FLAG code.
template <typename T, int = (T::Const(), 1)>
constexpr FlagHelpArg HelpArg(int) {
  return {FlagHelpMsg(T::Const()), FlagHelpKind::kLiteral};
}

template <typename T>
constexpr FlagHelpArg HelpArg(char) {
  return {FlagHelpMsg(&T::NonConst), FlagHelpKind::kGenFunc};
}

///////////////////////////////////////////////////////////////////////////////
// Flag default value auxiliary structs.

// Signature for the function generating the initial flag value (usually
// based on default value supplied in flag's definition)
using FlagDfltGenFunc = void* (*)();

union FlagDefaultSrc {
  constexpr explicit FlagDefaultSrc(FlagDfltGenFunc gen_func_arg)
      : gen_func(gen_func_arg) {}

  void* dynamic_value;
  FlagDfltGenFunc gen_func;
};

enum class FlagDefaultKind : uint8_t { kDynamicValue = 0, kGenFunc = 1 };

///////////////////////////////////////////////////////////////////////////////
// Flag current value auxiliary structs.

// The minimum atomic size we believe to generate lock free code, i.e. all
// trivially copyable types not bigger this size generate lock free code.
static constexpr int kMinLockFreeAtomicSize = 8;

// The same as kMinLockFreeAtomicSize but maximum atomic size. As double words
// might use two registers, we want to dispatch the logic for them.
#if defined(ABSL_FLAGS_INTERNAL_ATOMIC_DOUBLE_WORD)
static constexpr int kMaxLockFreeAtomicSize = 16;
#else
static constexpr int kMaxLockFreeAtomicSize = 8;
#endif

// We can use atomic in cases when it fits in the register, trivially copyable
// in order to make memcpy operations.
template <typename T>
struct IsAtomicFlagTypeTrait {
  static constexpr bool value =
      (sizeof(T) <= kMaxLockFreeAtomicSize &&
       type_traits_internal::is_trivially_copyable<T>::value);
};

// Clang does not always produce cmpxchg16b instruction when alignment of a 16
// bytes type is not 16.
struct alignas(16) FlagsInternalTwoWordsType {
  int64_t first;
  int64_t second;
};

constexpr bool operator==(const FlagsInternalTwoWordsType& that,
                          const FlagsInternalTwoWordsType& other) {
  return that.first == other.first && that.second == other.second;
}
constexpr bool operator!=(const FlagsInternalTwoWordsType& that,
                          const FlagsInternalTwoWordsType& other) {
  return !(that == other);
}

constexpr int64_t SmallAtomicInit() { return 0xababababababababll; }

template <typename T, typename S = void>
struct BestAtomicType {
  using type = int64_t;
  static constexpr int64_t AtomicInit() { return SmallAtomicInit(); }
};

template <typename T>
struct BestAtomicType<
    T, typename std::enable_if<(kMinLockFreeAtomicSize < sizeof(T) &&
                                sizeof(T) <= kMaxLockFreeAtomicSize),
                               void>::type> {
  using type = FlagsInternalTwoWordsType;
  static constexpr FlagsInternalTwoWordsType AtomicInit() {
    return {SmallAtomicInit(), SmallAtomicInit()};
  }
};

struct FlagValue {
  // Heap allocated value.
  void* dynamic = nullptr;
  // For some types, a copy of the current value is kept in an atomically
  // accessible field.
  union Atomics {
    // Using small atomic for small types.
    std::atomic<int64_t> small_atomic;
    template <typename T,
              typename K = typename std::enable_if<
                  (sizeof(T) <= kMinLockFreeAtomicSize), void>::type>
    int64_t load() const {
      return small_atomic.load(std::memory_order_acquire);
    }

#if defined(ABSL_FLAGS_INTERNAL_ATOMIC_DOUBLE_WORD)
    // Using big atomics for big types.
    std::atomic<FlagsInternalTwoWordsType> big_atomic;
    template <typename T, typename K = typename std::enable_if<
                              (kMinLockFreeAtomicSize < sizeof(T) &&
                               sizeof(T) <= kMaxLockFreeAtomicSize),
                              void>::type>
    FlagsInternalTwoWordsType load() const {
      return big_atomic.load(std::memory_order_acquire);
    }
    constexpr Atomics()
        : big_atomic{FlagsInternalTwoWordsType{SmallAtomicInit(),
                                               SmallAtomicInit()}} {}
#else
    constexpr Atomics() : small_atomic{SmallAtomicInit()} {}
#endif
  };
  Atomics atomics{};
};

///////////////////////////////////////////////////////////////////////////////
// Flag callback auxiliary structs.

// Signature for the mutation callback used by watched Flags
// The callback is noexcept.
// TODO(rogeeff): add noexcept after C++17 support is added.
using FlagCallbackFunc = void (*)();

struct FlagCallback {
  FlagCallbackFunc func;
  absl::Mutex guard;  // Guard for concurrent callback invocations.
};

///////////////////////////////////////////////////////////////////////////////
// Flag implementation, which does not depend on flag value type.
// The class encapsulates the Flag's data and access to it.

struct DynValueDeleter {
  explicit DynValueDeleter(FlagOpFn op_arg = nullptr) : op(op_arg) {}
  void operator()(void* ptr) const {
    if (op != nullptr) Delete(op, ptr);
  }

  const FlagOpFn op;
};

class FlagImpl {
 public:
  constexpr FlagImpl(const char* name, const char* filename, FlagOpFn op,
                     FlagMarshallingOpFn marshalling_op, FlagHelpArg help,
                     FlagDfltGenFunc default_value_gen)
      : name_(name),
        filename_(filename),
        op_(op),
        marshalling_op_(marshalling_op),
        help_(help.source),
        help_source_kind_(static_cast<uint8_t>(help.kind)),
        def_kind_(static_cast<uint8_t>(FlagDefaultKind::kGenFunc)),
        is_data_guard_inited_(false),
        modified_(false),
        on_command_line_(false),
        inited_(false),
        counter_(0),
        callback_(nullptr),
        default_src_(default_value_gen),
        data_guard_{} {}

  // Forces destruction of the Flag's data.
  void Destroy();

  // Constant access methods
  absl::string_view Name() const;
  std::string Filename() const;
  std::string Help() const;
  bool IsModified() const ABSL_LOCKS_EXCLUDED(*DataGuard());
  bool IsSpecifiedOnCommandLine() const ABSL_LOCKS_EXCLUDED(*DataGuard());
  std::string DefaultValue() const ABSL_LOCKS_EXCLUDED(*DataGuard());
  std::string CurrentValue() const ABSL_LOCKS_EXCLUDED(*DataGuard());
  void Read(void* dst, const FlagOpFn dst_op) const
      ABSL_LOCKS_EXCLUDED(*DataGuard());
  // Attempts to parse supplied `value` std::string. If parsing is successful, then
  // it replaces `dst` with the new value.
  bool TryParse(void** dst, absl::string_view value, std::string* err) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(*DataGuard());

#ifndef NDEBUG
  template <typename T>
  void Get(T* dst) const {
    Read(dst, &FlagOps<T>);
  }
#else
  template <typename T, typename std::enable_if<
                            !IsAtomicFlagTypeTrait<T>::value, int>::type = 0>
  void Get(T* dst) const {
    Read(dst, &FlagOps<T>);
  }
  // Overload for `GetFlag()` for types that support lock-free reads.
  template <typename T, typename std::enable_if<IsAtomicFlagTypeTrait<T>::value,
                                                int>::type = 0>
  void Get(T* dst) const {
    using U = BestAtomicType<T>;
    const typename U::type r = value_.atomics.template load<T>();
    if (r != U::AtomicInit()) {
      std::memcpy(static_cast<void*>(dst), &r, sizeof(T));
    } else {
      Read(dst, &FlagOps<T>);
    }
  }
#endif

  // Mutating access methods
  void Write(const void* src, const FlagOpFn src_op)
      ABSL_LOCKS_EXCLUDED(*DataGuard());
  bool SetFromString(absl::string_view value, FlagSettingMode set_mode,
                     ValueSource source, std::string* err)
      ABSL_LOCKS_EXCLUDED(*DataGuard());
  // If possible, updates copy of the Flag's value that is stored in an
  // atomic word.
  void StoreAtomic() ABSL_EXCLUSIVE_LOCKS_REQUIRED(*DataGuard());

  // Interfaces to operate on callbacks.
  void SetCallback(const FlagCallbackFunc mutation_callback)
      ABSL_LOCKS_EXCLUDED(*DataGuard());
  void InvokeCallback() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(*DataGuard());

  // Interfaces to save/restore mutable flag data
  template <typename T>
  std::unique_ptr<FlagStateInterface> SaveState(Flag<T>* flag) const
      ABSL_LOCKS_EXCLUDED(*DataGuard()) {
    T&& cur_value = flag->Get();
    absl::MutexLock l(DataGuard());

    return absl::make_unique<FlagState<T>>(
        flag, std::move(cur_value), modified_, on_command_line_, counter_);
  }
  bool RestoreState(const void* value, bool modified, bool on_command_line,
                    int64_t counter) ABSL_LOCKS_EXCLUDED(*DataGuard());

  // Value validation interfaces.
  void CheckDefaultValueParsingRoundtrip() const
      ABSL_LOCKS_EXCLUDED(*DataGuard());
  bool ValidateInputValue(absl::string_view value) const
      ABSL_LOCKS_EXCLUDED(*DataGuard());

 private:
  // Ensures that `data_guard_` is initialized and returns it.
  absl::Mutex* DataGuard() const ABSL_LOCK_RETURNED((absl::Mutex*)&data_guard_);
  // Returns heap allocated value of type T initialized with default value.
  std::unique_ptr<void, DynValueDeleter> MakeInitValue() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(*DataGuard());
  // Lazy initialization of the Flag's data.
  void Init();

  FlagHelpKind HelpSourceKind() const {
    return static_cast<FlagHelpKind>(help_source_kind_);
  }
  FlagDefaultKind DefaultKind() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(*DataGuard()) {
    return static_cast<FlagDefaultKind>(def_kind_);
  }

  // Immutable flag's state.

  // Flags name passed to ABSL_FLAG as second arg.
  const char* const name_;
  // The file name where ABSL_FLAG resides.
  const char* const filename_;
  // Type-specific handler.
  const FlagOpFn op_;
  // Marshalling ops handler.
  const FlagMarshallingOpFn marshalling_op_;
  // Help message literal or function to generate it.
  const FlagHelpMsg help_;
  // Indicates if help message was supplied as literal or generator func.
  const uint8_t help_source_kind_ : 1;

  // Mutable flag's state (guarded by `data_guard_`).

  // If def_kind_ == kDynamicValue, default_src_ holds a dynamically allocated
  // value.
  uint8_t def_kind_ : 1 ABSL_GUARDED_BY(*DataGuard());
  // Protects against multiple concurrent constructions of `data_guard_`.
  bool is_data_guard_inited_ : 1;
  // Has this flag's value been modified?
  bool modified_ : 1 ABSL_GUARDED_BY(*DataGuard());
  // Has this flag been specified on command line.
  bool on_command_line_ : 1 ABSL_GUARDED_BY(*DataGuard());

  // Indicates that the flag state is initialized.
  std::atomic<bool> inited_;
  // Mutation counter
  int64_t counter_ ABSL_GUARDED_BY(*DataGuard());
  // Optional flag's callback and absl::Mutex to guard the invocations.
  FlagCallback* callback_ ABSL_GUARDED_BY(*DataGuard());
  // Either a pointer to the function generating the default value based on the
  // value specified in ABSL_FLAG or pointer to the dynamically set default
  // value via SetCommandLineOptionWithMode. def_kind_ is used to distinguish
  // these two cases.
  FlagDefaultSrc default_src_ ABSL_GUARDED_BY(*DataGuard());
  // Current Flag Value
  FlagValue value_;

  // This is reserved space for an absl::Mutex to guard flag data. It will be
  // initialized in FlagImpl::Init via placement new.
  // We can't use "absl::Mutex data_guard_", since this class is not literal.
  // We do not want to use "absl::Mutex* data_guard_", since this would require
  // heap allocation during initialization, which is both slows program startup
  // and can fail. Using reserved space + placement new allows us to avoid both
  // problems.
  alignas(absl::Mutex) mutable char data_guard_[sizeof(absl::Mutex)];
};

///////////////////////////////////////////////////////////////////////////////
// The "unspecified" implementation of Flag object parameterized by the
// flag's value type.

template <typename T>
class Flag final : public flags_internal::CommandLineFlag {
 public:
  constexpr Flag(const char* name, const char* filename,
                 const FlagMarshallingOpFn marshalling_op,
                 const FlagHelpArg help,
                 const FlagDfltGenFunc default_value_gen)
      : impl_(name, filename, &FlagOps<T>, marshalling_op, help,
              default_value_gen) {}

  T Get() const {
    // See implementation notes in CommandLineFlag::Get().
    union U {
      T value;
      U() {}
      ~U() { value.~T(); }
    };
    U u;

    impl_.Get(&u.value);
    return std::move(u.value);
  }

  void Set(const T& v) { impl_.Write(&v, &FlagOps<T>); }

  void SetCallback(const FlagCallbackFunc mutation_callback) {
    impl_.SetCallback(mutation_callback);
  }

  // CommandLineFlag interface
  absl::string_view Name() const override { return impl_.Name(); }
  std::string Filename() const override { return impl_.Filename(); }
  absl::string_view Typename() const override { return ""; }
  std::string Help() const override { return impl_.Help(); }
  bool IsModified() const override { return impl_.IsModified(); }
  bool IsSpecifiedOnCommandLine() const override {
    return impl_.IsSpecifiedOnCommandLine();
  }
  std::string DefaultValue() const override { return impl_.DefaultValue(); }
  std::string CurrentValue() const override { return impl_.CurrentValue(); }
  bool ValidateInputValue(absl::string_view value) const override {
    return impl_.ValidateInputValue(value);
  }

  // Interfaces to save and restore flags to/from persistent state.
  // Returns current flag state or nullptr if flag does not support
  // saving and restoring a state.
  std::unique_ptr<FlagStateInterface> SaveState() override {
    return impl_.SaveState(this);
  }

  // Restores the flag state to the supplied state object. If there is
  // nothing to restore returns false. Otherwise returns true.
  bool RestoreState(const FlagState<T>& flag_state) {
    return impl_.RestoreState(&flag_state.cur_value_, flag_state.modified_,
                              flag_state.on_command_line_, flag_state.counter_);
  }
  bool SetFromString(absl::string_view value, FlagSettingMode set_mode,
                     ValueSource source, std::string* error) override {
    return impl_.SetFromString(value, set_mode, source, error);
  }
  void CheckDefaultValueParsingRoundtrip() const override {
    impl_.CheckDefaultValueParsingRoundtrip();
  }

 private:
  friend class FlagState<T>;

  void Destroy() override { impl_.Destroy(); }

  void Read(void* dst) const override { impl_.Read(dst, &FlagOps<T>); }
  FlagOpFn TypeId() const override { return &FlagOps<T>; }

  // Flag's implementation with value type abstracted out.
  FlagImpl impl_;
};

template <typename T>
inline void FlagState<T>::Restore() const {
  if (flag_->RestoreState(*this)) {
    ABSL_INTERNAL_LOG(INFO,
                      absl::StrCat("Restore saved value of ", flag_->Name(),
                                   " to: ", flag_->CurrentValue()));
  }
}

// This class facilitates Flag object registration and tail expression-based
// flag definition, for example:
// ABSL_FLAG(int, foo, 42, "Foo help").OnUpdate(NotifyFooWatcher);
template <typename T, bool do_register>
class FlagRegistrar {
 public:
  explicit FlagRegistrar(Flag<T>* flag) : flag_(flag) {
    if (do_register) flags_internal::RegisterCommandLineFlag(flag_);
  }

  FlagRegistrar& OnUpdate(FlagCallbackFunc cb) && {
    flag_->SetCallback(cb);
    return *this;
  }

  // Make the registrar "die" gracefully as a bool on a line where registration
  // happens. Registrar objects are intended to live only as temporary.
  operator bool() const { return true; }  // NOLINT

 private:
  Flag<T>* flag_;  // Flag being registered (not owned).
};

// This struct and corresponding overload to MakeDefaultValue are used to
// facilitate usage of {} as default value in ABSL_FLAG macro.
struct EmptyBraces {};

template <typename T>
T* MakeFromDefaultValue(T t) {
  return new T(std::move(t));
}

template <typename T>
T* MakeFromDefaultValue(EmptyBraces) {
  return new T;
}

}  // namespace flags_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_FLAGS_INTERNAL_FLAG_H_

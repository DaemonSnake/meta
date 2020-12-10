// RUN: %clang_cc1 -freflection -verify -std=c++1z %s

namespace meta {
  using info = decltype(reflexpr(void));
}

namespace non_template_reflection {
  constexpr meta::info invalid_refl = __invalid_reflection("custom error message");

  int decl_splice_test() {
    return [<invalid_refl>]; // expected-error {{cannot reify invalid reflection}} expected-note {{custom error message}}
  }

  int unqualid_test() {
    return unqualid(invalid_refl); // expected-error {{cannot reify invalid reflection}} expected-note {{custom error message}}
  }

  using ReflectedType = typename(invalid_refl); // expected-error {{cannot reify invalid reflection}} expected-note {{custom error message}}

  template<typename T>
  constexpr int foo() {
    return T();
  }

  constexpr int fcall_result = foo<templarg(invalid_refl)>(); // expected-error {{cannot reify invalid reflection}} expected-note {{custom error message}}

  constexpr int valueof_result = valueof(invalid_refl); // expected-error {{cannot reify invalid reflection}} expected-note {{custom error message}}
}

namespace template_reflection {
  template<int V>
  constexpr meta::info invalid_refl = __invalid_reflection(__concatenate("Error code: ", V));

  int decl_splice_test() {
    return [<invalid_refl<1>>]; // expected-error {{cannot reify invalid reflection}} expected-note {{Error code: 1}}
  }

  int unqualid_test() {
    return unqualid(invalid_refl<1>); // expected-error {{cannot reify invalid reflection}} expected-note {{Error code: 1}}
  }

  using ReflectedType = typename(invalid_refl<1>); // expected-error {{cannot reify invalid reflection}} expected-note {{Error code: 1}}

  template<typename T>
  constexpr int foo() {
    return T();
  }

  constexpr int fcall_result = foo<templarg(invalid_refl<1>)>(); // expected-error {{cannot reify invalid reflection}} expected-note {{Error code: 1}}

  constexpr int valueof_result = valueof(invalid_refl<1>); // expected-error {{cannot reify invalid reflection}} expected-note {{Error code: 1}}
}

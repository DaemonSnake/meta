// RUN: %clang_cc1 -std=c++1z -freflection %s

namespace meta {
  using info = decltype(reflexpr(void));
}

int global0 = 19;
const int global1 = 75;
constexpr int global2 = 76;

enum E { A, B, C };

struct S {
  static constexpr int value = 4;
  int num = 12;

  constexpr int f1() const { return 42; }
};

constexpr S s1;

int f() { return 0; }

template<meta::info refl, auto val>
void check_decl_splice_dependent_val() {
  static_assert([<refl>] == val);
}

template<meta::info refl, auto val>
void check_decl_splice_dependent_addr() {
  static_assert(&[<refl>] == val);
}

template<typename T>
void check_idexpr_dependent_member_ref() {
  static_assert(s1.idexpr(reflexpr(T::num)) == 12);
  static_assert(s1.idexpr(reflexpr(T::f1))() == 42);
}

int main() {
  // static_assert(idexpr(reflexpr(42)) == 42); // expected-error

  static_assert([<reflexpr(global1)>] == 75);
  check_decl_splice_dependent_val<reflexpr(global1), 75>();

  static_assert([<reflexpr(A)>] == 0);
  check_decl_splice_dependent_val<reflexpr(A), 0>();

  static_assert([<reflexpr(S::value)>] == 4);
  check_decl_splice_dependent_val<reflexpr(S::value), 4>();

  static_assert([<reflexpr(f)>] == &f);
  check_decl_splice_dependent_val<reflexpr(f), &f>();

  static_assert([<reflexpr(f)>] == f);
  check_decl_splice_dependent_val<reflexpr(f), f>();

  static_assert(&[<reflexpr(global0)>] == &global0);
  check_decl_splice_dependent_addr<reflexpr(global0), &global0>();

  static_assert(&[<reflexpr(S::value)>] == &S::value);
  check_decl_splice_dependent_addr<reflexpr(S::value), &S::value>();

  // static_assert(|reflexpr(s1.num)| == 12); // expected-error

  static_assert(s1.idexpr(reflexpr(S::num)) == 12);
  static_assert(s1.idexpr(reflexpr(S::f1))() == 42);
  check_idexpr_dependent_member_ref<S>();
}

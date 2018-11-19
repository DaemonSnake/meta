// RUN: %clangxx -std=c++1z -freflection %s

#define assert(E) if (!(E)) __builtin_abort();

constexpr auto fragment = __fragment namespace {
  int v_one = 1;
  int v_two = 2;

  int add_v_one(int num) {
    return v_one + num;
  }

  namespace bar {
    int v_eleven = 11;
  }
};

namespace foo {
  constexpr {
    -> fragment;
  }
};

int main() {
  assert(foo::v_one == 1);
  assert(foo::v_two == 2);
  assert(foo::add_v_one(1) == 2);

  assert(foo::bar::v_eleven == 11);

  return 0;
};

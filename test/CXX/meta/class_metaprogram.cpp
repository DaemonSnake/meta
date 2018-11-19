// RUN: %clangxx -std=c++1z -freflection %s

#include <experimental/meta>

int global_int = 42;

struct InternalFragClass {
  static int instance_count;

  InternalFragClass() {
    instance_count += 1;
  }

  ~InternalFragClass() {
    instance_count -= 1;
  }
};

int InternalFragClass::instance_count = 0;

constexpr auto inner_fragment = __fragment struct S {
  int* c0 = new int(5);
  int* c1;

  S() : c1(new int(10)) { }
  ~S() {
    delete c0;
    delete c1;
  }

  int inner_frag_num() {
    return 0;
  }

  int inner_proxy_frag_num() {
    return this->y;
  }

  int referenced_global() {
    return global_int;
  }
};

constexpr auto fragment = __fragment struct X {
  constexpr {
    -> inner_fragment;
  }

  InternalFragClass FragClass;
  int x = 1;
  int z = this->y;

  template<typename T>
  int get_z(const T& t) {
    return t.z;
  }

  int frag_num() {
    return 2;
  }

  int proxy_frag_num() {
    return this->y;
  }

  typedef int fragment_int;

  struct Nested {
    int bar;

    Nested(int bar) : bar(bar) { }

    int get_y(const X& t) {
      return t.y;
    }
  };
};

class Foo {
  int y = 55;

  constexpr {
    -> fragment;
  }

public:
  int dependent_on_injected_val() {
    return this->x;
  }
};

int main() {
  {
    Foo f;

    assert(f.x == 1);
    assert(f.dependent_on_injected_val() == 1);
    assert(f.frag_num() == 2);
    assert(f.inner_frag_num() == 0);
    assert(f.z == 55);
    assert(f.proxy_frag_num() == 55);
    assert(f.inner_proxy_frag_num() == 55);
    assert(f.referenced_global() == 42);
    assert(*f.c0 == 5);
    assert(*f.c1 == 10);
    assert(f.get_z(f) == 55);

    Foo::fragment_int int_of_injected_type = 1;
    assert(static_cast<int>(int_of_injected_type) == 1);

    assert(InternalFragClass::instance_count == 1);
  }

  {
    Foo f;
    Foo::Nested nested(5);

    assert(nested.bar == 5);
    assert(nested.get_y(f) == 55);
  }

  assert(InternalFragClass::instance_count == 0);

  return 0;
};

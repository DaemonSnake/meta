// RUN: %clang_cc1 -std=c++1z -freflection -verify %s
// expected-no-diagnostics

/// Tuple stuff

struct tuple
{
  int n = 10;
  float pi = 3.14;
  char c = 'a';
};

template<int N> struct getter { constexpr static void get(tuple const& t) { } };
template<> struct getter<0> { constexpr static int get(tuple const& t) { return t.n; } };
template<> struct getter<1> { constexpr static float get(tuple const& t) { return t.pi; } };
template<> struct getter<2> { constexpr static char get(tuple const& t) { return t.c; } };

template<int N> 
constexpr inline decltype(auto) 
get(tuple const& t) {
  return getter<N>::get(t);
}

namespace std
{
  template<typename T>
  struct tuple_size;

  template<typename T>
  struct tuple_size<T const> : tuple_size<T> { };

  template<typename T>
  struct tuple_size<T volatile> : tuple_size<T> { };

  template<typename T>
  struct tuple_size<T const volatile> : tuple_size<T> { };

  template<>
  struct tuple_size<::tuple>
  {
    static constexpr int value = 3;
  };

  template<typename I>
  constexpr int distance(I first, I limit) {
    int n = 0;
    while (first != limit) {
      ++n;
      ++first;
    }
    return n;
  }

  template<typename I>
  constexpr I next(I iter, int n = 1) {
    while (n != 0) {
      ++iter;
      --n;
    }
    return iter;
  }
};

// Range stuff

struct counter
{
  constexpr explicit counter(int n) : num(n) { }

  constexpr counter& operator++() { ++num; return *this; }
  constexpr counter operator++(int) { counter x(*this); ++num; return x; }

  constexpr int operator*() const { return num; }

  constexpr friend bool operator==(counter a, counter b) { return a.num == b.num; }
  constexpr friend bool operator!=(counter a, counter b) { return a.num != b.num; }
  
  int num;
};

template<int N>
struct range
{
  constexpr counter begin() const { return counter(0); }
  constexpr counter end() const { return counter(N); }

  // constexpr static int size() { return N; }
};

// Struct expansion stuff
template<typename T, typename S, typename U>
struct record {
  T t1 = T();
  S s1 = S();
  U u1 = U();

  struct nested {
    T skipped;
  };

  T t2 = T();
  U f() { return U(); }
  S s2 = S();
};



// Tests

void test_array() {
  int arr[] = { 1, 2, 3 };
  for... (int a : arr)
    ;
}

void test_constexpr_array() {
  static constexpr int arr[] = { 1, 2, 3, 4 };
  for... (constexpr int a : arr)
    ;
}

constexpr int global_arr[] = { 0, 1 };
void test_constexpr_array_2() {
  for... (constexpr int a : global_arr)
    ;
}

void test_tuple() {
  tuple tup;
  for... (auto x : tup)
    ;
}

template <typename T, typename U, typename S>
void test_record() {
  record<T, U, S> r;
  for... (auto x : r)
    ;
}

void test_constexpr_tuple() {
  static constexpr tuple tup;
  for... (constexpr auto x : tup)
    ;
}

void test_constexpr_range() {
  static constexpr range<7> ints;
  for... (constexpr int n : ints)
    ;
}

template <typename T, typename U, typename S>
void test_constexpr_record() {
  static constexpr record<T, U, S> r;
  for... (constexpr auto field : r)
    ;
}

int main() {
  test_array();
  test_tuple();
  test_record<int, double, char>();
  test_constexpr_array();
  test_constexpr_array_2();
  test_constexpr_tuple();
  test_constexpr_range();
  test_constexpr_record<int, double, char>();
}


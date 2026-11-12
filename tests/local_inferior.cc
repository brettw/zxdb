// A small DWARF fixture for the integrated Linux debugger smoke test.
__attribute__((noinline)) int StandaloneCompute(int value) {
  volatile int result = value + 7;
  return result;
}

int main() { return StandaloneCompute(35) == 42 ? 0 : 1; }

# Qubic Lite Core Linux Build

## Prerequisites
To be able to compile the core code with clang the following prerequisites have to be meet:

> Clang >= 18.1.0
> cmake >= 3.14
> nasm >= 2.16.03

Potentially it also works with lower versions but isn't tested yet.

## Compiler Installation Commands

```
sudo apt update && upgrade
sudo apt install -y build-essential clang cmake nasm
sudo apt install -y libboost-stacktrace-dev libc++-dev libc++abi-dev libjsoncpp-dev uuid-dev zlib1g-dev libzstd-dev g++ libstdc++-12-dev libfmt-dev
```

## Compilation
Currently compilation with cmake and clang is only supported on Linux systems. Window and OSX might work but is not properly tested.
For a example compilation execute the following commands:

1.  **Navigate to the Project Root:**
    Open your terminal and change the directory to the root of the project where the main `CMakeLists.txt` file is located.

    ```bash
    cd /path/to/your/project
    ```

2.  **Create and Enter Build Directory:**
    It's standard practice to perform an "out-of-source" build. Create a build directory (e.g., `build`) and navigate into it.

    ```bash
    # Create the build directory if it doesn't exist
    mkdir -p build

    # Enter the build directory
    cd build
    ```

3. **Configure project**
    Instruct cmake to use `clang` and `clang++` as well configure the rest of the build.

    ```bash
    # In your build directory
    cmake .. -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++ -D BUILD_TESTS:BOOL=OFF -D BUILD_BINARY:BOOL=ON -D CMAKE_BUILD_TYPE=Release -D ENABLE_AVX512=OFF
    ```

    There are a few option to set during configuration:

* **`-D BUILD_TESTS=<ON|OFF>`**
    * **Values:** `ON`, `OFF`
    * **Meaning:** `ON` builds the test suite, `OFF` skips building tests.

* **`-D BUILD_BINARY=<ON|OFF>`**
    * **Values:** `ON`, `OFF`
    * **Meaning:** `ON` builds the EFI file, `OFF` skips building EFI file. Currently this build is not working.

* **`-D CMAKE_BUILD_TYPE=<Type>`**
    * **Values:** `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`
    * **Meaning:** Sets the build mode for optimization and debug info (e.g., `Debug` for debugging, `Release` for performance).

* **`-D ENABLE_AVX512=<ON|OFF>`**
    * **Values:** `ON`, `OFF`
    * **Meaning:** `ON` enables code using AVX-512 CPU instructions (requires support), `OFF` disables it.

4.  **Build the Project:**
    Use the CMake `--build` command to invoke the underlying build tool (like `make` or `ninja`).

    ```bash
    # Build the project using the configuration generated in the previous step
    cmake --build .

    # Optional: Build in parallel (e.g., using 4 cores if using make)
    # cmake --build . -- -j4
    # Or if using Ninja (often the default if installed), it usually uses all cores by default
    ```

The output binary will be located at `build/src`

## Running Tests

### Configure (once)

Use a dedicated build directory to avoid mixing test and release artifacts:

```bash
cd /path/to/core-lite

cmake -S . -B build-test \
  -DCMAKE_CXX_COMPILER=clang-18 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DBUILD_BINARY=OFF \
  -DUSE_SANITIZER=OFF
```

### Compile

```bash
cmake --build build-test --target qubic_core_tests -j$(nproc)
```

> **Known linker issue on Ubuntu (clang-18 + GCC stdlib mix):**
> The link step may fail with `undefined reference to '__cxa_pure_virtual'` or
> `vtable for __cxxabiv1::...`. Work around it by appending `-lc++abi -lstdc++`
> manually after compilation completes:
>
> ```bash
> cd build-test/test
> eval "$(cat CMakeFiles/qubic_core_tests.dir/link.txt) -lc++abi -lstdc++"
> ```
>
> Prerequisites: `sudo apt install libc++abi-dev libstdc++-12-dev`

### Run all tests

```bash
./build-test/test/qubic_core_tests
```

### Run QSB contract tests only

```bash
./build-test/test/qubic_core_tests --gtest_filter="ContractTestingQSB*"
```

### Run a single test

```bash
./build-test/test/qubic_core_tests --gtest_filter="ContractTestingQSB.TestOverrideLock_BlockedAfterMaxAttempts"
```

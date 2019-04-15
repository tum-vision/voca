# Project setup

### Set up editor

We support [VS Code](https://code.visualstudio.com/download) with the [clangd
extension](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
In general any C++ editor will work, specially if it supports `clangd` or the
[`build/compile_commands.json`](https://clang.llvm.org/docs/JSONCompilationDatabase.html)
file that is created after the `cmake -B build` step.

### Set up clang-format and clangd-tidy

We use clang-format. Since different versions produce slightly different output,
we use a specific one that you can find in the [format.sh](/scripts/format.sh)
script (usually something that is found in all of the supported OS package
managers). Make sure to setup that version in your editor, or run format.sh
before commiting.

We use [clangd-tidy](https://github.com/lljbash/clangd-tidy) instead of
clang-tidy to make the checks be fast and match with the warnings shown in the
editor using clangd. The original codebase did not use these checks, therefore
many files have warnings. We are trying to fix them little by little, so each
time a MR is submitted avoid introducing new warnings and it is appreciated if
you also fix old warnings in the modified files. To run clangd-tidy you just:

```bash
sudo apt install clangd
pip install clangd-tidy
./scripts/clangd-tidy-diff-file.sh # To run clang-tidy only on the files differing from the main branch
./scripts/clangd-tidy-all.sh # To run clang-tidy on all files
```

## FAQ

### How to improve build times

- `ccache`: Set up ccache and make sure cmake is picking it up to avoid
  rebuilding files.
- `mold`: Install mold for faster link times.
- Compile single cmake targets; e.g., `cmake --build build --target basalt_vio`
- Try to keep your edits to source `.cpp` files. Editing header `.h` files, will
  trigger recompilation of all files including this header. If you find yourself
  editing a header too often, consider moving its functionality to a cpp file.
- Have a good multi-core cpu: To improve build times for initial builds, and for
  when you edit a header included in many places.
- Have a good single-core cpu: To improve single file build times.

### How to use a debugger?

When using the `--preset development`, binaries are compiled with RelWithDebInfo mode
which includes debug info. So you can run any of the binaries through gdb: `gdb --args
./build/calibration --dataset-path data/euroc_calib/`

If you want to debug within vscode, install the [C/C++
extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools).
Disable its intellisense so that it doesn't collide with clangd by setting
`"C_Cpp.intelliSenseEngine": "disabled"` in vscode settings.json.
Go to `run and debug`, and `open launch.json` to edit the default launch.json
file in the `.vscode` directory. You can use a launch.json file like the
following and edit its tasks to create a new one with the binary and arguments
you need to debug. Then press F5 to debug.

```json
// .vscode/launch.json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "cppdbg",
      "request": "launch",
      "name": "Debug basalt_vio",
      "program": "${workspaceFolder}/build/basalt_vio",
      "args": [
        "--dataset-type",
        "euroc",
        "--dataset-path",
        "MOO09_short_down/",
        "--config-path",
        "data/msd/msdmo_config.json"
        "--cam-calib",
        "data/msd/msdmo_calib.json"
      ],
      "cwd": "${workspaceFolder}"
    },
    {
      "type": "cppdbg",
      "request": "launch",
      "name": "Debug basalt_mapper",
      "program": "${workspaceFolder}/build/basalt_mapper",
      "cwd": "${workspaceFolder}"
    }
  ]
}
```

### How to use address sanitizer to find issues with my code?

You can use [ASan](https://github.com/google/sanitizers/wiki/addresssanitizer)
by configuring the project with `cmake -B build
-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer -fsanitize=undefined
-fsanitize=address`. ASan reports useful information about your program on
runtime whenever any of its runtime checks detect an issue. Be aware that it
will likely also report issues from dependencies.

### My code is crashing without a good reason?

Congratulations! you just hit undefined behavior likely caused by Eigen.

The usage of a header-only library like Eigen has some nice advantages, but an
annoying disadvantage is that since many parts of the dependency chain include
the eigen headers, they each can configure it differently and cause ABI
incompatibilities.

This project has some cmake rules set up to keep the different eigen usages
compatible with each other, but you might be inadvertedly using your system's
eigen installation (a.k.a., installed through `apt install libeigen3-dev`
for example), and it might be a different version.

To check that this is not the case:

- Check the CMake output or the `build/compile_commands.json` file to ensure that
  all dependencies are using the same eigen submodule under `thirdparty/eigen`.
- Be sure to use `#include` the right eigen files. If you are writing
  `#include <eigen3/Eigen/...> instead of `#include <Eigen/...>` that can be
  looking up your system-wide eigen installation instead of the submodule. You can
  usually check that the include is working by using your editor's "go to
  definition" functionality on the include line and checking that the file comes
  from the eigen submodule.

If you want to read more about possible eigen issues take a look at these pages

1. [Eigen Memory Issues](https://github.com/ethz-asl/eigen_catkin/wiki/Eigen-Memory-Issues)
2. [Eigen: Common Pitfalls](https://eigen.tuxfamily.org/dox/TopicPitfalls.html)

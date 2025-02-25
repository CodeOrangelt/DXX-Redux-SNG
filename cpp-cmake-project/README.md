# README.md

# C++ CMake Project

This project is a C++ application configured with CMake. It includes various build presets for different environments and configurations.

## Project Structure

- `CMakeLists.txt`: Contains the build configuration for the CMake project.
- `CMakePresets.json`: Defines presets for configuring the project, including settings for Debug and Release builds.
- `src/main.cpp`: The entry point of the application.

## Building the Project

To build the project, follow these steps:

1. Open a terminal and navigate to the project directory.
2. Create a build directory:
   ```
   mkdir build
   cd build
   ```
3. Configure the project using CMake:
   ```
   cmake .. --preset <preset-name>
   ```
   Replace `<preset-name>` with the desired preset (e.g., `x86-debug`, `x86-release`, `linux-debug`).

4. Build the project:
   ```
   cmake --build .
   ```

## Running the Application

After building the project, you can run the application from the build directory:
```
./<executable-name>
```
Replace `<executable-name>` with the name of the compiled executable.
# raycasting

![showcase](assets/showcase.png)

## 💡 Overview
This project is a quick showcase of the classic Wolfenstein 3D raycasting technique built in C.  
It traces rays through a 2D map to determine wall heights and colors, creating a pseudo-3D view.  
The engine uses a Digital Differential Analyzer (DDA) approach to step through the map grid, detect wall hits, and compute distances.

>[!Note]
>This is a rewrite of my `raycasting-rs` Rust version. You can find the repository [here](https://github.com/adrior11/raycasting-rs/tree/main).

### Inspiration
- [YouTube video by jdh](https://www.youtube.com/watch?v=fSjc8vLMg8c&t)
- [Blogpost by Lode](https://lodev.org/cgtutor/raycasting.html).

## 📦 Prerequisites
- **C Compiler:** GCC or Clang
- **Make:** for building
- **SDL2 Library:** development headers and libraries

### Installing SDL2

#### macOS
Homebrew example:
```bash
$ brew install sdl2
```

#### Linux
Ubuntu/Debian example:
```bash
$ sudo apt-get install libsdl2-dev
```
Fedora example:
```bash
$ sudo dnf install SDL2-devel
```
Arch example:
```bash
$ sudo pacman -S sdl2
```

## 🚀 Building and Running
1. Clone the repository:
``` bash
$ git clone https://github.com/adrior11/raycasting.git
$ cd raycasting
```

2. Build and Run the application:
```bash
$ make
$ ./raycasting
```

Optionally, generate `compile_commands.json` for IDEs and code-indexing tools:
```bash
$ bear -- make
```

## 🎮 Controls
- **W / S**: Move forward / backward
- **A / D**: Strafe left / right
- **← / →**: Rotate camera
- **ESC**: Quit the application

## 🗺️ Map Format
`map.txt` begins with two numbers indicating the width and height of the map, followed by `width * height` integers for tile types:
```
10 10
1 1 1 1 1 1 1 1 1 1
1 0 0 0 0 0 0 0 0 1
1 0 0 2 0 0 3 3 0 1
...
```

## License
This project is available under the MIT License.

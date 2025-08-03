# Joystick Controller

This is a GTK+ 3-based application that allows controlling an on-screen virtual keyboard and mouse using a joystick. The program supports two modes: **Keyboard Mode** for navigating and interacting with an on-screen keyboard, and **Game Mode** for simulating WASD keys and mouse movements. The joystick's right analog stick controls mouse movement, with swapped X and Y axes, and specific buttons are mapped to mouse clicks and wheel scrolling.

## Features
- **Keyboard Mode**:
  - Displays an on-screen virtual keyboard.
  - Navigate the keyboard using the joystick's left analog stick.
  - Select keys with the right thumb button.
  - Move the mouse cursor using the right analog stick (with swapped axes).
- **Game Mode**:
  - Hides the keyboard and simulates WASD keys for movement using the left analog stick.
  - Controls the mouse cursor with the right analog stick (with swapped axes).
- **Mouse Controls**:
  - Left click: `BTN_TL` (event code 310).
  - Right click: `BTN_TR` (event code 311).
  - Mouse wheel up: `BTN_TL2` (event code 312).
  - Mouse wheel down: `BTN_TR2` (event code 313).
- **Mode Switching**: Toggle between Keyboard and Game modes using `BTN_MODE`.

## Dependencies
To compile and run the program, you need the following libraries:
- **GTK+ 3**: For the graphical user interface (on-screen keyboard).
- **libevdev**: For reading joystick input events.
- **pthread**: For multithreading support.
- **Linux input subsystem**: For simulating mouse and keyboard events via `/dev/uinput`.

On Debian-based systems (e.g., Ubuntu), install the dependencies with:
```bash
sudo apt-get update
sudo apt-get install libgtk-3-dev libevdev-dev

g++ -o joystick_controller joystick_controller.cpp $(pkg-config --cflags --libs gtk+-3.0 libevdev) -pthread

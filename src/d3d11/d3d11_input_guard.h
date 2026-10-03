#pragma once

namespace dxvk {

  /**
   * \brief Keeps the game from holding the mouse while the Remix UI is open
   *
   * Window-message blocking (the swap chain WndProc hook) does not reach games
   * that read input through DirectInput, or that pin the cursor themselves:
   * an exclusive DirectInput mouse hides the cursor and confines it to the
   * window, and engines re-centre and clip it every frame. While the UI is
   * open this guard releases DirectInput devices and feeds the game empty
   * input, and ignores the game's ClipCursor / SetCursorPos calls. When the
   * UI closes the game reacquires its devices through its normal lost-device
   * handling.
   */
  namespace D3D11InputGuard {

    /// Installs the DirectInput and cursor hooks once per process.
    void install();

    /// Called every present with whether the Remix UI currently blocks game input.
    void setBlocking(bool blocking);

    bool isBlocking();

  }

}

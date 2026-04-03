/**
 * @file stack.h
 * @brief Core model layer for scroller layout state.
 */
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <hyprutils/math/Vector2D.hpp>

#include "../core/core.h"
#include "../core/types.h"
#include "../list.h"

namespace ScrollerModel {

// Global width fractions management
void set_width_fractions(const std::vector<double> &fractions);
const std::vector<double> &get_width_fractions();

enum class WindowHeight {
  OneThird,
  OneHalf,
  TwoThirds,
  One,
  Number,
  Free,
  Auto
};

enum class Reorder { Auto, Lazy };

class Window {
public:
  Window(PHLWINDOW window, double box_h);
  PHLWINDOWREF ptr() const;
  double get_geom_h() const;
  double get_geom_y() const;
  void set_geom_h(double geom_h);
  void set_geom_y(double geom_y);
  void push_geom();
  void pop_geom();
  WindowHeight get_height() const;
  void update_height(WindowHeight h, double max);
  void set_height_free();

private:
  struct Memory {
    double box_y;
    double box_h;
  };
  PHLWINDOWREF window;
  WindowHeight height;
  double box_y;
  double box_h;
  Memory mem;
};

class Stack {
public:
  Stack(PHLWINDOW cwindow, double maxw, double maxh, Mode mode);
  Stack(std::unique_ptr<Window> window, size_t widthIndex, double maxw,
        double maxh, Mode mode);
  ~Stack();

  bool get_init() const;
  void set_init();
  size_t size() const;

  bool has_window(PHLWINDOW window) const;
  bool swap_windows(PHLWINDOW a, PHLWINDOW b);
  template <typename Fn> void for_each_window(Fn &&fn) const {
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
      if (auto window = win->data()->ptr().lock())
        std::forward<Fn>(fn)(window);
    }
  }
  void remove_window(PHLWINDOW window);
  void focus_window(PHLWINDOW window);

  double get_geom_x() const;
  double get_geom_y() const;
  double get_geom_w() const;
  double get_geom_h() const;
  void set_geom_w(double w);
  void set_geom_h(double h);
  Vector2D get_height() const;

  void scale(const Vector2D &bmin, const Vector2D &start, double scale,
             double gap);
  bool toggle_fullscreen(const ScrollerCore::Box &fullbbox);
  void set_fullscreen(const ScrollerCore::Box &fullbbox);
  bool expanded() const;
  void push_geom();
  void pop_geom();
  void toggle_maximized(double maxw, double maxh);

  bool fullscreen() const;
  bool maximized() const;
  Mode get_mode() const;
  void set_mode(Mode mode, double maxw, double maxh);
  void shift_local_geometry(double delta);
  void set_geom_pos(double x, double y);

  void recalculate_stack_geometry(const Vector2D &gap_x, double gap);
  PHLWINDOW get_active_window();
  bool active_at_edge(Direction direction) const;
  void move_active(Direction direction);
  FocusMoveResult move_focus(Direction direction, bool focus_wrap);

  void admit_window(std::unique_ptr<Window> window);
  void restore_window(std::unique_ptr<Window> window, bool insertBeforeActive);
  std::unique_ptr<Window> expel_active(double gap);
  void align_window(Direction direction, double gap);

  // Dynamic width API
  size_t get_width_index() const;
  void set_width_index(size_t index);
  bool is_free() const;
  void set_free();
  double get_current_fraction() const;
  void update_width(size_t index, double maxw, double maxh);
  void update_width_to_fraction(double fraction, double maxw, double maxh);
  void cycle_width(int step, double maxw, double maxh);
  void fit_size(FitSize fitsize, const Vector2D &gap_x, double gap);
  void resize_active_window(double maxw, const Vector2D &gap_x, double gap,
                            const Vector2D &delta);

#ifdef COLORS_IPC
  std::string get_width_name() const;
  std::string get_height_name() const;
#endif

private:
  ListNode<Window *> *findWindowNode(PHLWINDOW window) const;
  void adjust_windows(ListNode<Window *> *win, const Vector2D &gap_x,
                      double gap);

  struct Memory {
    ScrollerCore::Box geom;
  };

  size_t widthIndex;
  bool isFree;
  Mode mode;
  WindowHeight height;
  Reorder reorder;
  bool initialized;
  ScrollerCore::Box geom;
  bool fullscreened = false;
  bool maxdim;
  Memory mem;
  ScrollerCore::Box full;
  ListNode<Window *> *active = nullptr;
  List<Window *> windows;
};

} // namespace ScrollerModel

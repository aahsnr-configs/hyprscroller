/**
 * @file stack.cpp
 * @brief Implementation of stack-level geometry, movement, and sizing logic.
 */
#include "stack.h"

#include <algorithm>
#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprlang.hpp>
#include <spdlog/spdlog.h>

#include "../core/interval.h"
#include "../core/layout_math.h"
#include "../core/layout_profile.h"

extern HANDLE PHANDLE;

namespace ScrollerModel {
namespace {
std::vector<double> s_widthFractions;
}

void set_width_fractions(const std::vector<double> &fractions) {
  s_widthFractions = fractions;
}

const std::vector<double> &get_width_fractions() { return s_widthFractions; }

namespace {
double stack_local_origin(const ScrollerCore::Box &geom, Mode mode) {
  return mode == Mode::Column ? geom.x : geom.y;
}
double stack_local_span(const ScrollerCore::Box &geom, Mode mode) {
  return mode == Mode::Column ? geom.w : geom.h;
}
double stack_cross_span(const ScrollerCore::Box &geom, Mode mode) {
  return mode == Mode::Column ? geom.h : geom.w;
}
double stack_primary_span(const ScrollerCore::Box &geom, Mode mode) {
  return mode == Mode::Column ? geom.h : geom.w;
}
double local_viewport_end(const ScrollerCore::Box &geom, Mode mode) {
  return stack_local_origin(geom, mode) + stack_local_span(geom, mode);
}

Vector2D compose_window_position(const ScrollerCore::Box &geom, Mode mode,
                                 double border, const Vector2D &cross_gap,
                                 double local_pos, double local_gap) {
  if (mode == Mode::Column)
    return Vector2D(local_pos + border + local_gap,
                    geom.y + border + cross_gap.x);
  return Vector2D(geom.x + border + cross_gap.x,
                  local_pos + border + local_gap);
}

Vector2D compose_window_size(const ScrollerCore::Box &geom, Mode mode,
                             double border, const Vector2D &cross_gap,
                             double local_size, double local_gap0,
                             double local_gap1) {
  const auto cross_size = std::max(stack_cross_span(geom, mode) - 2.0 * border -
                                       cross_gap.x - cross_gap.y,
                                   1.0);
  const auto main_size =
      std::max(local_size - 2.0 * border - local_gap0 - local_gap1, 1.0);
  if (mode == Mode::Column)
    return Vector2D(main_size, cross_size);
  return Vector2D(cross_size, main_size);
}

double preset_extent(double fraction, double max) { return max * fraction; }

size_t initial_width_index(PHLWINDOW window, bool &outIsFree) {
  static auto const *column_default_width =
      (Hyprlang::STRING const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:scroller:column_default_width")
          ->getDataStaticPtr();
  const std::string preset = *column_default_width;
  outIsFree = false;
  if (preset == "maximized" || preset == "floating") {
    outIsFree = true;
    return 0;
  }
  std::string str = preset;
  str.erase(std::remove_if(str.begin(), str.end(), ::isspace), str.end());
  size_t comma = str.find(',');
  std::string firstToken =
      (comma == std::string::npos) ? str : str.substr(0, comma);
  double frac = -1.0;
  if (firstToken == "onethird")
    frac = 1.0 / 3.0;
  else if (firstToken == "onehalf")
    frac = 0.5;
  else if (firstToken == "twothirds")
    frac = 2.0 / 3.0;
  else {
    try {
      frac = std::stod(firstToken);
      frac = std::clamp(frac, 0.01, 1.0);
    } catch (...) {
    }
  }
  if (frac > 0.0 && !s_widthFractions.empty()) {
    size_t bestIdx = 0;
    double bestDiff = std::abs(s_widthFractions[0] - frac);
    for (size_t i = 1; i < s_widthFractions.size(); ++i) {
      double diff = std::abs(s_widthFractions[i] - frac);
      if (diff < bestDiff) {
        bestDiff = diff;
        bestIdx = i;
      }
    }
    return bestIdx;
  }
  return 0;
}

static bool is_window_fully_visible(Window *window,
                                    const ScrollerCore::Box &geom, Mode mode) {
  if (!window)
    return false;
  const auto p0 = std::round(window->get_geom_y());
  const auto p1 = std::round(window->get_geom_y() + window->get_geom_h());
  return ScrollerCore::Interval::fully_visible(
      p0, p1, stack_local_origin(geom, mode), local_viewport_end(geom, mode));
}
static bool is_window_intersect_viewport(Window *window,
                                         const ScrollerCore::Box &geom,
                                         Mode mode) {
  if (!window)
    return false;
  const auto p0 = window->get_geom_y();
  const auto p1 = window->get_geom_y() + window->get_geom_h();
  return ScrollerCore::Interval::intersects(
      p0, p1, stack_local_origin(geom, mode), local_viewport_end(geom, mode));
}
static void sync_window_target_geometry(PHLWINDOW window) {
  if (!window)
    return;
  const auto target = window->layoutTarget();
  if (!target)
    return;
  target->setPositionGlobal(
      Hyprutils::Math::CBox(window->m_position, window->m_size));
}
} // namespace

// ============================================================================
// Stack implementation
// ============================================================================

Stack::Stack(PHLWINDOW cwindow, double maxw, double maxh, Mode mode)
    : mode(mode), height(WindowHeight::One), reorder(Reorder::Auto),
      initialized(false), maxdim(false) {
  bool isFree = false;
  widthIndex = initial_width_index(cwindow, isFree);
  this->isFree = isFree;
  Window *window = new Window(cwindow, mode == Mode::Column ? maxw : maxh);
  update_width(widthIndex, maxw, maxh);
  windows.push_back(window);
  active = windows.first();
}

Stack::Stack(std::unique_ptr<Window> window, size_t widthIndex, double maxw,
             double maxh, Mode mode)
    : widthIndex(widthIndex), isFree(false), mode(mode),
      height(WindowHeight::One), reorder(Reorder::Auto), initialized(true),
      maxdim(false) {
  update_width(widthIndex, maxw, maxh);
  if (!window)
    return;
  window->set_geom_h(stack_local_span(geom, mode));
  windows.push_back(window.release());
  active = windows.first();
}

Stack::~Stack() {
  for (auto win = windows.first(); win != nullptr; win = win->next())
    delete win->data();
  windows.clear();
}

bool Stack::get_init() const { return initialized; }
void Stack::set_init() { initialized = true; }
size_t Stack::size() const { return windows.size(); }

ListNode<Window *> *Stack::findWindowNode(PHLWINDOW window) const {
  if (!window)
    return nullptr;
  for (auto win = windows.first(); win != nullptr; win = win->next())
    if (win->data()->ptr().lock() == window)
      return win;
  return nullptr;
}

bool Stack::has_window(PHLWINDOW window) const {
  return findWindowNode(window) != nullptr;
}

bool Stack::swap_windows(PHLWINDOW a, PHLWINDOW b) {
  if (a == b)
    return false;
  auto *na = findWindowNode(a), *nb = findWindowNode(b);
  if (!na || !nb)
    return false;
  windows.swap(na, nb);
  if (active == na)
    active = nb;
  else if (active == nb)
    active = na;
  return true;
}

void Stack::remove_window(PHLWINDOW window) {
  reorder = Reorder::Auto;
  auto *win = findWindowNode(window);
  if (!win)
    return;
  if (active && window == active->data()->ptr().lock())
    active = active != windows.last() ? active->next() : active->prev();
  auto *removed = win->data();
  windows.erase(win);
  delete removed;
  if (windows.size() != 1 || !active)
    return;
  active->data()->update_height(WindowHeight::One,
                                stack_local_span(geom, mode));
}

void Stack::focus_window(PHLWINDOW window) {
  if (auto *win = findWindowNode(window))
    active = win;
}

double Stack::get_geom_x() const { return geom.x; }
double Stack::get_geom_y() const { return geom.y; }
double Stack::get_geom_w() const { return geom.w; }
double Stack::get_geom_h() const { return geom.h; }
void Stack::set_geom_w(double w) { geom.w = w; }
void Stack::set_geom_h(double h) { geom.h = h; }

Vector2D Stack::get_height() const {
  if (windows.empty())
    return Vector2D(geom.y, geom.y);
  if (mode == Mode::Column)
    return Vector2D(geom.y, geom.y + geom.h);
  Vector2D height;
  auto *first = windows.first()->data(), *last = windows.last()->data();
  height.x = first->get_geom_y();
  height.y = last->get_geom_y() + last->get_geom_h();
  return height;
}

void Stack::scale(const Vector2D &bmin, const Vector2D &start, double scale,
                  double gap) {
  for (auto win = windows.first(); win != nullptr; win = win->next()) {
    const auto oldLocal = win->data()->get_geom_y();
    const auto newLocal = mode == Mode::Column
                              ? start.x + (oldLocal - bmin.x) * scale
                              : start.y + (oldLocal - bmin.y) * scale;
    win->data()->set_geom_y(newLocal);
    win->data()->set_geom_h(win->data()->get_geom_h() * scale);
    PHLWINDOW window = win->data()->ptr().lock();
    if (!window)
      continue;
    auto border = window->getRealBorderSize();
    auto gap0 = win == windows.first() ? 0.0 : gap;
    auto gap1 = win == windows.last() ? 0.0 : gap;
    if (mode == Mode::Column) {
      window->m_position = Vector2D(win->data()->get_geom_y() + border + gap0,
                                    start.y + border + geom.y - bmin.y);
      window->m_size.x =
          (window->m_size.x + 2.0 * border + gap0 + gap1) * scale - gap0 -
          gap1 - 2.0 * border;
      window->m_size.y *= scale;
    } else {
      window->m_position = Vector2D(start.x + border + geom.x - bmin.x,
                                    win->data()->get_geom_y() + border + gap0);
      window->m_size.x *= scale;
      window->m_size.y =
          (window->m_size.y + 2.0 * border + gap0 + gap1) * scale - gap0 -
          gap1 - 2.0 * border;
    }
    window->m_size = Vector2D(std::max(window->m_size.x, 1.0),
                              std::max(window->m_size.y, 1.0));
    sync_window_target_geometry(window);
  }
}

bool Stack::toggle_fullscreen(const ScrollerCore::Box &fullbbox) {
  full = fullbbox;
  if (!active)
    return false;
  fullscreened = !fullscreened;
  if (fullscreened) {
    mem.geom = geom;
    if (mode == Mode::Column)
      geom.h = fullbbox.h;
    else
      geom.w = fullbbox.w;
  } else
    geom = mem.geom;
  return fullscreened;
}

void Stack::set_fullscreen(const ScrollerCore::Box &fullbbox) {
  full = fullbbox;
}
void Stack::push_geom() {
  mem.geom = geom;
  for (auto w = windows.first(); w != nullptr; w = w->next())
    w->data()->push_geom();
}
void Stack::pop_geom() {
  geom = mem.geom;
  for (auto w = windows.first(); w != nullptr; w = w->next())
    w->data()->pop_geom();
}
void Stack::toggle_maximized(double maxw, double maxh) {
  if (!active)
    return;
  maxdim = !maxdim;
  if (maxdim) {
    mem.geom = geom;
    active->data()->push_geom();
    if (mode == Mode::Column) {
      geom.h = maxh;
      active->data()->set_geom_h(maxw);
    } else {
      geom.w = maxw;
      active->data()->set_geom_h(maxh);
    }
  } else {
    geom = mem.geom;
    active->data()->pop_geom();
  }
}
bool Stack::fullscreen() const {
  return active && active->data()->ptr().lock()->isFullscreen();
}
bool Stack::expanded() const { return fullscreened; }
bool Stack::maximized() const { return maxdim; }
Mode Stack::get_mode() const { return mode; }
void Stack::set_mode(Mode nextMode, double maxw, double maxh) {
  if (mode == nextMode)
    return;
  mode = nextMode;
  update_width(widthIndex, maxw, maxh);
  if (active && size() == 1) {
    active->data()->set_geom_h(stack_local_span(geom, mode));
    active->data()->set_geom_y(stack_local_origin(geom, mode));
  }
}
void Stack::shift_local_geometry(double delta) {
  if (delta == 0.0)
    return;
  for (auto win = windows.first(); win != nullptr; win = win->next())
    win->data()->set_geom_y(win->data()->get_geom_y() + delta);
}
void Stack::set_geom_pos(double x, double y) { geom.set_pos(x, y); }
void Stack::recalculate_stack_geometry(const Vector2D &gap_x, double gap) {
  if (!active)
    return;
  if (fullscreen()) {
    PHLWINDOW wactive = active->data()->ptr().lock();
    if (!wactive)
      return;
    active->data()->set_geom_y(stack_local_origin(full, mode));
    wactive->m_position = Vector2D(full.x, full.y);
    wactive->m_size = Vector2D(full.w, full.h);
    sync_window_target_geometry(wactive);
    return;
  }
  Window *wactive = active->data();
  PHLWINDOW win = wactive->ptr().lock();
  if (!win)
    return;
  const auto viewportStart = stack_local_origin(geom, mode);
  const auto viewportEnd = local_viewport_end(geom, mode);
  const auto clampEnd = viewportEnd - wactive->get_geom_h();
  const auto a0 = std::round(wactive->get_geom_y());
  const auto a1 = std::round(wactive->get_geom_y() + wactive->get_geom_h());
  if (a0 < viewportStart) {
    wactive->set_geom_y(viewportStart);
    adjust_windows(active, gap_x, gap);
    return;
  }
  if (a1 > viewportEnd) {
    wactive->set_geom_y(clampEnd);
    adjust_windows(active, gap_x, gap);
    return;
  }
  if (reorder != Reorder::Auto) {
    adjust_windows(active, gap_x, gap);
    return;
  }
  Window *prev = active->prev() ? active->prev()->data() : nullptr;
  Window *next = active->next() ? active->next()->data() : nullptr;
  const bool prev_visible = is_window_fully_visible(prev, geom, mode);
  const bool next_visible = is_window_fully_visible(next, geom, mode);
  if (prev_visible || next_visible) {
    adjust_windows(active, gap_x, gap);
    return;
  }
  const auto nextSize = next ? next->get_geom_h() : 0.0;
  const auto prevSize = prev ? prev->get_geom_h() : 0.0;
  const auto activeSize = wactive->get_geom_h();
  const double newPos =
      mode == Mode::Column
          ? ScrollerCore::choose_anchor_x(next != nullptr, prev != nullptr,
                                          activeSize, nextSize, prevSize,
                                          wactive->get_geom_y(), geom)
          : ScrollerCore::choose_anchor_y(next != nullptr, prev != nullptr,
                                          activeSize, nextSize, prevSize, geom);
  wactive->set_geom_y(newPos);
  adjust_windows(active, gap_x, gap);
}
PHLWINDOW Stack::get_active_window() {
  return active ? active->data()->ptr().lock() : nullptr;
}
bool Stack::active_at_edge(Direction direction) const {
  if (!active)
    return false;
  switch (direction) {
  case Direction::Left:
    return mode == Mode::Column && active == windows.first();
  case Direction::Right:
    return mode == Mode::Column && active == windows.last();
  case Direction::Up:
    return mode == Mode::Row && active == windows.first();
  case Direction::Down:
    return mode == Mode::Row && active == windows.last();
  default:
    return false;
  }
}
void Stack::move_active(Direction direction) {
  if (!active)
    return;
  const auto backward = ScrollerCore::stack_item_backward_direction(mode);
  const auto forward = ScrollerCore::stack_item_forward_direction(mode);
  if (direction == backward && active != windows.first()) {
    reorder = Reorder::Auto;
    auto previous = active->prev();
    windows.swap(active, previous);
  } else if (direction == forward && active != windows.last()) {
    reorder = Reorder::Auto;
    auto next = active->next();
    windows.swap(active, next);
  }
}
FocusMoveResult Stack::move_focus(Direction direction, bool focus_wrap) {
  if (!active)
    return FocusMoveResult::NoOp;
  const auto backward = ScrollerCore::stack_item_backward_direction(mode);
  const auto forward = ScrollerCore::stack_item_forward_direction(mode);
  if (direction == backward && active != windows.first()) {
    reorder = Reorder::Auto;
    active = active->prev();
    return FocusMoveResult::Moved;
  }
  if (direction == forward && active != windows.last()) {
    reorder = Reorder::Auto;
    active = active->next();
    return FocusMoveResult::Moved;
  }
  auto monitorDirection = [&]() -> Math::eDirection {
    switch (direction) {
    case Direction::Left:
      return Math::fromChar('l');
    case Direction::Right:
      return Math::fromChar('r');
    case Direction::Up:
      return Math::fromChar('u');
    default:
      return Math::fromChar('d');
    }
  }();
  if (g_pCompositor->getMonitorInDirection(monitorDirection) != nullptr)
    return FocusMoveResult::CrossMonitor;
  auto previous = active;
  if (focus_wrap)
    active = direction == backward ? windows.last() : windows.first();
  return active != previous ? FocusMoveResult::Moved : FocusMoveResult::NoOp;
}
void Stack::admit_window(std::unique_ptr<Window> window) {
  reorder = Reorder::Auto;
  if (!window)
    return;
  if (active) {
    const auto activeWindow = active->data();
    window->set_geom_h(activeWindow->get_geom_h());
    window->set_geom_y(activeWindow->get_geom_y() + activeWindow->get_geom_h());
  } else {
    window->set_geom_h(stack_local_span(geom, mode));
    window->set_geom_y(stack_local_origin(geom, mode));
  }
  active = windows.emplace_after(active, window.release());
}
void Stack::restore_window(std::unique_ptr<Window> window,
                           bool insertBeforeActive) {
  reorder = Reorder::Auto;
  if (!window)
    return;
  if (!active) {
    window->set_geom_h(stack_local_span(geom, mode));
    window->set_geom_y(stack_local_origin(geom, mode));
    active = windows.emplace_after(active, window.release());
    return;
  }
  active = insertBeforeActive ? windows.emplace_before(active, window.release())
                              : windows.emplace_after(active, window.release());
}
std::unique_ptr<Window> Stack::expel_active(double /*gap*/) {
  reorder = Reorder::Auto;
  if (!active)
    return {};
  std::unique_ptr<Window> window(active->data());
  auto act = active == windows.first() ? active->next() : active->prev();
  windows.erase(active);
  active = act;
  return window;
}
void Stack::align_window(Direction direction, double /*gap*/) {
  if (!active)
    return;
  const auto backward = ScrollerCore::stack_item_backward_direction(mode);
  const auto forward = ScrollerCore::stack_item_forward_direction(mode);
  const auto localOrigin = stack_local_origin(geom, mode);
  const auto localSpan = stack_local_span(geom, mode);
  switch (direction) {
  case Direction::Left:
  case Direction::Up:
    if (direction != backward)
      break;
    reorder = Reorder::Lazy;
    active->data()->set_geom_y(localOrigin);
    break;
  case Direction::Right:
  case Direction::Down:
    if (direction != forward)
      break;
    reorder = Reorder::Lazy;
    active->data()->set_geom_y(localOrigin + localSpan -
                               active->data()->get_geom_h());
    break;
  case Direction::Center:
    reorder = Reorder::Lazy;
    active->data()->set_geom_y(
        localOrigin + 0.5 * (localSpan - active->data()->get_geom_h()));
    break;
  default:
    break;
  }
}
size_t Stack::get_width_index() const { return widthIndex; }
void Stack::set_width_index(size_t index) {
  if (index < s_widthFractions.size()) {
    widthIndex = index;
    isFree = false;
  }
}
bool Stack::is_free() const { return isFree; }
void Stack::set_free() { isFree = true; }
double Stack::get_current_fraction() const {
  if (isFree)
    return 1.0;
  if (widthIndex < s_widthFractions.size())
    return s_widthFractions[widthIndex];
  return 0.5;
}
void Stack::update_width(size_t index, double maxw, double maxh) {
  widthIndex = index;
  isFree = false;
  double fraction = get_current_fraction();
  if (mode == Mode::Column) {
    geom.w = maxw;
    geom.h = maximized() ? maxh : preset_extent(fraction, maxh);
  } else {
    geom.w = maximized() ? maxw : preset_extent(fraction, maxw);
    geom.h = maxh;
  }
}
void Stack::update_width_to_fraction(double fraction, double maxw,
                                     double maxh) {
  isFree = true;
  if (mode == Mode::Column)
    geom.h = fraction * maxh;
  else
    geom.w = fraction * maxw;
}
void Stack::cycle_width(int step, double maxw, double maxh) {
  if (s_widthFractions.empty())
    return;
  if (isFree) {
    double currentFrac =
        (mode == Mode::Column) ? (geom.h / maxh) : (geom.w / maxw);
    currentFrac = std::clamp(currentFrac, 0.01, 1.0);
    size_t bestIdx = 0;
    double bestDiff = std::abs(s_widthFractions[0] - currentFrac);
    for (size_t i = 1; i < s_widthFractions.size(); ++i) {
      double diff = std::abs(s_widthFractions[i] - currentFrac);
      if (diff < bestDiff) {
        bestDiff = diff;
        bestIdx = i;
      }
    }
    widthIndex = bestIdx;
    isFree = false;
  } else {
    int newIndex = static_cast<int>(widthIndex) + step;
    if (newIndex < 0)
      newIndex = static_cast<int>(s_widthFractions.size()) - 1;
    else if (newIndex >= static_cast<int>(s_widthFractions.size()))
      newIndex = 0;
    widthIndex = static_cast<size_t>(newIndex);
  }
  update_width(widthIndex, maxw, maxh);
}
void Stack::fit_size(FitSize fitsize, const Vector2D &gap_x, double gap) {
  reorder = Reorder::Auto;
  ListNode<Window *> *from = nullptr, *to = nullptr;
  switch (fitsize) {
  case FitSize::Active:
    from = to = active;
    break;
  case FitSize::Visible:
    for (auto w = windows.first(); w != nullptr; w = w->next())
      if (is_window_intersect_viewport(w->data(), geom, mode)) {
        from = w;
        break;
      }
    for (auto w = windows.last(); w != nullptr; w = w->prev())
      if (is_window_intersect_viewport(w->data(), geom, mode)) {
        to = w;
        break;
      }
    break;
  case FitSize::All:
    from = windows.first();
    to = windows.last();
    break;
  case FitSize::ToEnd:
    from = active;
    to = windows.last();
    break;
  case FitSize::ToBeg:
    from = windows.first();
    to = active;
    break;
  default:
    return;
  }
  if (from && to) {
    double total = 0.0;
    for (auto c = from; c != to->next(); c = c->next())
      total += c->data()->get_geom_h();
    if (total <= 0.0)
      return;
    for (auto c = from; c != to->next(); c = c->next()) {
      Window *win = c->data();
      win->set_height_free();
      win->set_geom_h(win->get_geom_h() / total * stack_local_span(geom, mode));
    }
    from->data()->set_geom_y(stack_local_origin(geom, mode));
    adjust_windows(from, gap_x, gap);
  }
}
void Stack::resize_active_window(double maxw, const Vector2D &gap_x, double gap,
                                 const Vector2D &delta) {
  if (!active)
    return;
  PHLWINDOW activeWindow = active->data()->ptr().lock();
  if (!activeWindow)
    return;
  auto border = activeWindow->getRealBorderSize();
  const auto stackDelta = mode == Mode::Column ? delta.y : delta.x;
  const auto windowDelta = mode == Mode::Column ? delta.x : delta.y;
  auto renderedStackSpan = stack_primary_span(geom, mode) + stackDelta -
                           2.0 * border - gap_x.x - gap_x.y;
  auto maxStackSpan =
      stack_primary_span(geom, mode) + stackDelta -
      2.0 * (border + std::max(std::max(gap_x.x, gap_x.y), gap));
  const auto maxPrimary = mode == Mode::Column ? full.h : maxw;
  if (maxStackSpan <= 0.0 || renderedStackSpan >= maxPrimary)
    return;
  if (std::abs(static_cast<int>(windowDelta)) > 0) {
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
      auto gap0 = win == windows.first() ? 0.0 : gap;
      auto gap1 = win == windows.last() ? 0.0 : gap;
      auto wh = win->data()->get_geom_h() - gap0 - gap1 - 2.0 * border;
      if (win == active)
        wh += windowDelta;
      if (wh <= 0.0 ||
          wh + 2.0 * border + gap0 + gap1 > stack_local_span(geom, mode))
        return;
    }
  }
  reorder = Reorder::Auto;
  set_free();
  if (mode == Mode::Column)
    geom.h += stackDelta;
  else
    geom.w += stackDelta;
  if (std::abs(static_cast<int>(windowDelta)) > 0) {
    for (auto win = windows.first(); win != nullptr; win = win->next())
      if (win == active)
        win->data()->set_geom_h(win->data()->get_geom_h() + windowDelta);
  }
}
void Stack::adjust_windows(ListNode<Window *> *win, const Vector2D &gap_x,
                           double gap) {
  if (!win)
    return;
  for (auto w = win->prev(), p = win; w != nullptr; p = w, w = w->prev())
    w->data()->set_geom_y(p->data()->get_geom_y() - w->data()->get_geom_h());
  for (auto w = win->next(), p = win; w != nullptr; p = w, w = w->next())
    w->data()->set_geom_y(p->data()->get_geom_y() + p->data()->get_geom_h());
  auto anchorWindow = win ? win->data()->ptr().lock() : nullptr;
  auto monitor =
      anchorWindow ? g_pCompositor->getMonitorFromID(anchorWindow->monitorID())
                   : nullptr;
  const auto fullStart = monitor
                             ? (mode == Mode::Column ? monitor->m_position.x
                                                     : monitor->m_position.y)
                             : stack_local_origin(geom, mode);
  const auto fullEnd =
      monitor ? fullStart + (mode == Mode::Column ? monitor->m_size.x
                                                  : monitor->m_size.y)
              : local_viewport_end(geom, mode);
  const auto reservedBefore =
      std::max(0.0, stack_local_origin(geom, mode) - fullStart);
  const auto reservedAfter =
      std::max(0.0, fullEnd - local_viewport_end(geom, mode));
  size_t shiftedAbove = 0, shiftedBelow = 0;
  for (auto w = windows.first(); w != nullptr; w = w->next()) {
    auto *wdata = w->data();
    const auto boxStart = wdata->get_geom_y();
    const auto boxEnd = boxStart + wdata->get_geom_h();
    if (reservedBefore > 0.0 && boxEnd <= stack_local_origin(geom, mode)) {
      wdata->set_geom_y(boxStart - reservedBefore);
      shiftedAbove++;
    } else if (reservedAfter > 0.0 &&
               boxStart >= local_viewport_end(geom, mode)) {
      wdata->set_geom_y(boxStart + reservedAfter);
      shiftedBelow++;
    }
  }
  for (auto w = windows.first(); w != nullptr; w = w->next()) {
    PHLWINDOW window = w->data()->ptr().lock();
    if (!window)
      continue;
    auto gap0 = w == windows.first() ? 0.0 : gap;
    auto gap1 = w == windows.last() ? 0.0 : gap;
    auto border = window->getRealBorderSize();
    const auto localSize = w->data()->get_geom_h();
    window->m_position = compose_window_position(geom, mode, border, gap_x,
                                                 w->data()->get_geom_y(), gap0);
    window->m_size =
        compose_window_size(geom, mode, border, gap_x, localSize, gap0, gap1);
    sync_window_target_geometry(window);
  }
}

#ifdef COLORS_IPC
std::string Stack::get_width_name() const {
  if (isFree)
    return "Free";
  if (widthIndex < s_widthFractions.size()) {
    double f = s_widthFractions[widthIndex];
    if (std::abs(f - 1.0 / 3.0) < 0.01)
      return "OneThird";
    if (std::abs(f - 0.5) < 0.01)
      return "OneHalf";
    if (std::abs(f - 2.0 / 3.0) < 0.01)
      return "TwoThirds";
    return "Custom";
  }
  return "";
}
std::string Stack::get_height_name() const {
  switch (height) {
  case WindowHeight::Auto:
    return "Auto";
  case WindowHeight::Free:
    return "Free";
  default:
    return "";
  }
}
#endif

} // namespace ScrollerModel

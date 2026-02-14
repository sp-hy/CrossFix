#pragma once

#include <string>
#include <unordered_map>

struct ViewportRect {
  int x;
  int y;
  int width;
  int height;
  float overridefactor;
};

class RoomData {
private:
  static const std::unordered_map<std::string, ViewportRect> data;

public:
  static const ViewportRect *get(const std::string &name);
};

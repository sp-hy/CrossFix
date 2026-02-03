#pragma once

#include <unordered_map>
#include <string>

struct ViewportRect {
    int x;
    int y;
    int width;
    int height;
};

class RoomData {
private:
    static const std::unordered_map<std::string, ViewportRect> data;
    
public:
    static const ViewportRect* get(const std::string& name);
};

// Viewport Utilities - Shared viewport manipulation logic

#define NOMINMAX
#include "viewport_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ViewportUtils {
UINT CopyViewportsToBuffer(D3D11_VIEWPORT *outBuffer, UINT bufferSize,
                           const D3D11_VIEWPORT *pViewports,
                           UINT numViewports) {
  if (!outBuffer || !pViewports || bufferSize == 0 || numViewports == 0) {
    return 0;
  }

  UINT count = std::min(numViewports, bufferSize);
  memcpy(outBuffer, pViewports, sizeof(D3D11_VIEWPORT) * count);
  return count;
}

// Sentinel value: use -1 for any field to match any value (wildcard)
constexpr float WILDCARD = -1.0f;

// Viewport definition for matching and transforming specific UI elements
struct ViewportDefinition {
  float baseX;     // Base X position for repositioning (normalized, <1280)
  float originalX; // Original X position (can be >1280, -1 means same as baseX)
  float y;         // Y coordinate to match (WILDCARD = any)
  float width;     // Width to match (WILDCARD = any, e.g. loading bars)
  float height;    // Height to match (WILDCARD = any)
  float epsilon;   // Tolerance for matching
};

void ApplyViewportWidescreenFix(D3D11_VIEWPORT *viewports, UINT count,
                                float widescreenRatio) {
  if (!viewports || count == 0) {
    return;
  }

  // Define viewport configurations to handle
  // Add more entries here for different UI elements in the future
  const ViewportDefinition definitions[] = {
      // Format: { baseX, originalX, y, width, height, epsilon }

      // Stamina bars - 1 person team
      {716.0f, 1996.0f, 792.0f, 212.0f, 20.0f, 1.0f},

      // Stamina bars - 2 person team
      {588.0f, 1868.0f, 792.0f, 472.0f, 20.0f, 1.0f},

      // Stamina bars - 3 person team
      {460.0f, 1740.0f, 792.0f, 724.0f, 20.0f, 1.0f},

      // Battle UI Menu
      {112.0f, 1392.0f, 640.0f, 272.0f, 208.0f, 1.0f},

      // Battle UI - Command Menu
      {352.0f, 1632.0f, 672.0f, 32.0f, 144.0f, 1.0f},
      {112.0f, 1392.0f, 672.0f, 32.0f, 144.0f, 1.0f},
      {248.0f, 1528.0f, 816.0f, 104.0f, 32.0f, 1.0f},
      {144.0f, 1424.0f, 816.0f, 104.0f, 32.0f, 1.0f},
      {248.0f, 1528.0f, 640.0f, 104.0f, 32.0f, 1.0f},
      {144.0f, 1424.0f, 640.0f, 104.0f, 32.0f, 1.0f},
      {248.0f, 1528.0f, 672.0f, 104.0f, 144.0f, 1.0f},
      {144.0f, 1424.0f, 672.0f, 104.0f, 144.0f, 1.0f},

      // Menu Customise Selectors
      {1108.0f, 2388.0f, 232.0f, 720.0f, 752.0f, 1.0f},

      // Loading Bar
      {896.0f, 2176.0f, 992.0f, WILDCARD, 32.0f, 1.0f},

      // Character name legacy portrait screen
      {64.0f, 1344.0, 0.0, 256.0f, 1792.0f, 1.0f},

      // Menu item underlines
      {1088.0f, 2368.0f, 1452.0f, 496.0f, 184.0f, 1.0f},
      {1088.0f, 2368.0f, 136.0f, 872.0f, 1500.0f, 1.0f},
      {1088.0f, 2368.0f, 1260.0f, 500.0f, 376.0f, 1.0f},
      {1576.0f, 1576.0f, 136.0f, 384.0f, 4.0f, 1.0f},
      {1728.0f, 1728.0f, 136.0f, 232.0f, 4.0f, 1.0f},
      {1196.0f, 1196.0f, 1260.0f, 392.0f, 184.0f, 1.0f},
      {1488.0f, 1488.0f, 340.0f, 264.0f, 164.0f, 1.0f},
      {1684.0f, 1684.0f, 504.0f, 32.0f, 4.0f, 1.0f},
      {1616.0f, 1616.0f, 136.0f, 344.0f, 4.0f, 1.0f},

      // Menu item underlines
      {1160.0f, 1160.0f, 1384.0f, 388.0f, 120.0f, 1.0f},
      {1116.0f, 1116.0f, 1384.0f, 440.0f, 180.0f, 1.0f},
      {1088.0f, 1088.0f, 1452.0f, 496.0f, 184.0f, 1.0f},

      // Add more viewport definitions here as needed:
      // { BaseX, OriginalX, Y, Width, Height, Epsilon },

  };

  const int numDefinitions = sizeof(definitions) / sizeof(definitions[0]);

  for (UINT i = 0; i < count; ++i) {
    bool matched = false;

    // Check against all defined viewport configurations
    for (int d = 0; d < numDefinitions; d++) {
      const ViewportDefinition &def = definitions[d];

      // Check if viewport matches this definition
      // Match on Width, Y, and Height (WILDCARD = -1 skips that field)
      bool widthMatch =
          (def.width < 0) || (std::abs(viewports[i].Width - def.width) < 0.1f);
      bool yMatch = (def.y < 0) ||
                    (std::abs(viewports[i].TopLeftY - def.y) < def.epsilon);
      bool heightMatch =
          (def.height < 0) ||
          (std::abs(viewports[i].Height - def.height) < def.epsilon);
      bool dimensionsMatch = widthMatch && yMatch && heightMatch;

      if (!dimensionsMatch)
        continue;

      // Now check X coordinate - it can be either the baseX or originalX
      float incomingX = viewports[i].TopLeftX;
      bool xMatches = (std::abs(incomingX - def.baseX) < def.epsilon);

      // If originalX is set (not -1), also check against that
      if (!xMatches && def.originalX > 0.0f) {
        xMatches = (std::abs(incomingX - def.originalX) < def.epsilon);
      }

      bool isMatch = dimensionsMatch && xMatches;

      if (isMatch) {
        matched = true;

        // Store original X before width scaling
        float originalX = viewports[i].TopLeftX;

        // Apply width scaling
        viewports[i].Width *= widescreenRatio;

        // Calculate repositioned X based on base position
        float xOffset = originalX - def.baseX;
        viewports[i].TopLeftX = (def.baseX * widescreenRatio) + xOffset;

        break;
      }
    }
  }
}
} // namespace ViewportUtils

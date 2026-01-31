// Viewport Utilities - Shared viewport manipulation logic

#define NOMINMAX
#include "viewport_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ViewportUtils {
    UINT CopyViewportsToBuffer(
        D3D11_VIEWPORT* outBuffer,
        UINT bufferSize,
        const D3D11_VIEWPORT* pViewports,
        UINT numViewports
    ) {
        if (!outBuffer || !pViewports || bufferSize == 0 || numViewports == 0) {
            return 0;
        }

        UINT count = std::min(numViewports, bufferSize);
        memcpy(outBuffer, pViewports, sizeof(D3D11_VIEWPORT) * count);
        return count;
    }

    // Viewport definition for matching and transforming specific UI elements
    struct ViewportDefinition {
        float baseX;       // Base X position for repositioning (normalized, <1280)
        float originalX;   // Original X position (can be >1280, -1 means same as baseX)
        float y;           // Y coordinate to match
        float width;       // Width to match (original unscaled width)
        float height;      // Height to match
        float epsilon;     // Tolerance for matching
    };

    void ApplyStaminaBarWidescreenFix(
        D3D11_VIEWPORT* viewports,
        UINT count,
        float widescreenRatio
    ) {
        if (!viewports || count == 0) {
            return;
        }

        // Define viewport configurations to handle
        // Add more entries here for different UI elements in the future
        const ViewportDefinition definitions[] = {
            // Format: { baseX, originalX, y, width, height, epsilon }
            
            // Stamina bars - 1 person team (BaseX=716, Y=792, W=212, H=20)
            { 716.0f, 1996.0f, 792.0f, 212.0f, 20.0f, 1.0f },  // 716 + 1280 = 1996

            // Stamina bars - 2 person team (BaseX=588, Y=792, W=472, H=20)
            { 588.0f, 1868.0f, 792.0f, 472.0f, 20.0f, 1.0f },  // 588 + 1280 = 1868
            
            // Stamina bars - 3 person team (BaseX=460, Y=792, W=724, H=20)
            // Note: Original X is 1740 (outside 1280 buffer), normalized to 1740-1280=460
            { 460.0f, 1740.0f, 792.0f, 724.0f, 20.0f, 1.0f },


            // Battle UI Menu (BaseX=112, Y=640, W=272, H=208)
            { 112.0f, 1392.0f, 640.0f, 272.0f, 208.0f, 1.0f },  // 112 + 1280 = 1392
            
            // Battle UI - Additional pieces (Y=672 with different sizes)
            { 352.0f, 1632.0f, 672.0f, 32.0f, 144.0f, 1.0f },  // 352 + 1280 = 1632 Right narrow bar
            { 112.0f, 1392.0f, 672.0f, 32.0f, 144.0f, 1.0f },  // 112 + 1280 = 1392 Left narrow bar
            
            // Battle UI - Additional pieces (Y=816)
            { 248.0f, 1528.0f, 816.0f, 104.0f, 32.0f, 1.0f },  // 248 + 1280 = 1528
            { 144.0f, 1424.0f, 816.0f, 104.0f, 32.0f, 1.0f },  // 144 + 1280 = 1424
            
            // Battle UI - Additional pieces (Y=640)
            { 248.0f, 1528.0f, 640.0f, 104.0f, 32.0f, 1.0f },  // 248 + 1280 = 1528
            { 144.0f, 1424.0f, 640.0f, 104.0f, 32.0f, 1.0f },  // 144 + 1280 = 1424
            
            // Battle UI - Additional pieces (Y=672 with larger sizes)
            { 248.0f, 1528.0f, 672.0f, 104.0f, 144.0f, 1.0f },  // 248 + 1280 = 1528 Right wide bar
            { 144.0f, 1424.0f, 672.0f, 104.0f, 144.0f, 1.0f },  // 144 + 1280 = 1424 Left wide bar

            
            // Add more viewport definitions here as needed:
            // { BaseX, OriginalX, Y, Width, Height, Epsilon },
        };
        
        const int numDefinitions = sizeof(definitions) / sizeof(definitions[0]);

        for (UINT i = 0; i < count; ++i) {
            bool matched = false;
            
            // Check against all defined viewport configurations
            for (int d = 0; d < numDefinitions; d++) {
                const ViewportDefinition& def = definitions[d];
                
                // Check if viewport matches this definition
                // Match on Width, Y, and Height first
                bool dimensionsMatch = (std::abs(viewports[i].Width - def.width) < 0.1f &&
                                       std::abs(viewports[i].TopLeftY - def.y) < def.epsilon &&
                                       std::abs(viewports[i].Height - def.height) < def.epsilon);
                
                if (!dimensionsMatch) continue;
                
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
}

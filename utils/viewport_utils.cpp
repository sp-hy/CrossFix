// Viewport Utilities - Shared viewport manipulation logic

#define NOMINMAX
#include "viewport_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

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
        float baseX;       // Base X position for repositioning
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
            // Stamina bars - 1 person team (BaseX=716, Y=792, W=212, H=20)
            { 716.0f, 792.0f, 212.0f, 20.0f, 1.0f },

            // Stamina bars - 2 person team (BaseX=588, Y=792, W=472, H=20)
            { 588.0f, 792.0f, 472.0f, 20.0f, 1.0f },
            
            // Stamina bars - 3 person team (BaseX=460, Y=792, W=724, H=20)
            // Note: Original X is 1740 (outside 1280 buffer), normalized to 1740-1280=460
            { 460.0f, 792.0f, 724.0f, 20.0f, 1.0f },
            
            // Add more viewport definitions here as needed:
            // { BaseX, Y, Width, Height, Epsilon },
        };
        
        const int numDefinitions = sizeof(definitions) / sizeof(definitions[0]);

        for (UINT i = 0; i < count; ++i) {
            bool matched = false;
            
            // Check against all defined viewport configurations
            for (int d = 0; d < numDefinitions; d++) {
                const ViewportDefinition& def = definitions[d];
                
                // Check if viewport matches this definition (Width, Y, and Height)
                // By checking width, we ensure we only process original unscaled viewports
                bool isMatch = (std::abs(viewports[i].Width - def.width) < 0.1f &&
                               std::abs(viewports[i].TopLeftY - def.y) < def.epsilon &&
                               std::abs(viewports[i].Height - def.height) < def.epsilon);
                
                if (isMatch) {
                    matched = true;
                    
                    // Store original X before width scaling
                    float originalX = viewports[i].TopLeftX;
                    
                    // Apply width scaling
                    viewports[i].Width *= widescreenRatio;
                    
                    // Calculate repositioned X based on base position (OLD METHOD)
                    float xOffset = originalX - def.baseX;
                    viewports[i].TopLeftX = (def.baseX * widescreenRatio) + xOffset;
                    
                    // Debug logging
                    std::cout << "PROCESSED: Original X=" << originalX << " W=" << def.width 
                              << " -> New X=" << viewports[i].TopLeftX << " W=" << viewports[i].Width << std::endl;
                    
                    break;
                }
            }
        }
    }
}

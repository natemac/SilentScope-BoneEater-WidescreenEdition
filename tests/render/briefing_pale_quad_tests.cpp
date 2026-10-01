#include "render/briefing_pale_quad.h"
#include <cstdlib>
using namespace bone_eater::render;
int main(){
 if(!matchesBriefingPaleAsset(briefingPaleVertices,briefingPaleMaterial))return 1;
 // Every geometry and material byte participates in identification. Changes
 // must fall through to the original renderer, never hide a near-match asset.
 for(size_t i=0;i<briefingPaleVertices.size();++i){auto v=briefingPaleVertices;v[i]^=1;if(matchesBriefingPaleAsset(v,briefingPaleMaterial))return 2;}
 for(size_t i=0;i<briefingPaleMaterial.size();++i){auto m=briefingPaleMaterial;m[i]^=1;if(matchesBriefingPaleAsset(briefingPaleVertices,m))return 3;}
 if(matchesBriefingPaleAsset({},{}))return 4;
}

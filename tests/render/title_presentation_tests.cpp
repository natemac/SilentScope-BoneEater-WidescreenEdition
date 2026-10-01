#include "render/title_presentation_policy.h"
#include "render/title_art_rotation.h"
#include "render/news_presentation_policy.h"
#include "render/auxiliary_retry_policy.h"
#include <cassert>
#include <limits>
#include <initializer_list>
using bone_eater::render::TitlePresentationPolicy;
int main() {
    assert(bone_eater::render::titleArtSuffix(0)==L"00.png");
    assert(bone_eater::render::titleArtSuffix(9)==L"09.png");
    assert(bone_eater::render::titleArtSuffix(10)==L"10.png");
    for(unsigned seed : {1u,42u,999u}) {
        bone_eater::render::TitleArtRotation art(seed);
        assert(art.menu()==0);
        art.observe(1,15);assert(art.title()==0);
        unsigned previous=99; bool differsFromSequential=false;
        for(unsigned bag=0;bag<20;++bag) {
            bool seen[12] {};
            for(unsigned loop=0;loop<12;++loop) {
                const auto priorMenu=art.menu();
                art.observe(1,6);const auto selected=art.title();
                assert(selected<12&&!seen[selected]&&selected!=previous);
                seen[selected]=true;previous=selected;
                differsFromSequential|=selected!=loop;
                assert(art.menu()==priorMenu); // Unpresented selection never changes menu.
                art.observe(0,7);assert(art.title()==selected);
                for(unsigned frame=0;frame<100;++frame)art.observe(1,7);
                assert(art.title()==selected);
                art.presented();assert(art.menu()==selected);
                for(unsigned fade : {8u,15u,16u})art.observe(1,fade);
                assert(art.title()==selected&&art.menu()==selected);
                art.observe(1,12);
            }
        }
        assert(differsFromSequential);
        art.observe(2,7);auto selected=art.title();art.presented();
        art.leave();art.observe(2,7);
        assert(art.title()!=selected&&art.menu()==selected);
    }

    bone_eater::render::NewsPresentationPolicy news;
    assert(news.alpha(5,100,1)==0);
    assert(std::abs(news.alpha(5,500,1)-.5f)<.001f);
    assert(news.alpha(5,900,1)==1);
    assert(news.alpha(5,950,.25f)==.25f); // Native fade continues after child layout teardown.
    assert(news.alpha(5,1000,0)==0);
    news.reset();assert(news.alpha(5,1100,1)==0);
    using bone_eater::render::auxiliarySharedPause;
    assert(auxiliarySharedPause(800,810));
    assert(auxiliarySharedPause(2000,2000));
    assert(!auxiliarySharedPause(2001,2001));
    assert(!auxiliarySharedPause(20,800));
    assert(!auxiliarySharedPause(800,20));
    assert(!auxiliarySharedPause(800,1000));
    assert(!auxiliarySharedPause(0,0));
    TitlePresentationPolicy p;
    assert(!p.admit(1,15,1)); // Start from attract must not flash custom artwork.
    assert(p.admit(1,6,0));
    assert(p.admit(1,6,.3f));
    assert(p.admit(1,7,1));
    assert(p.admit(1,8,.2f));
    assert(!p.admit(1,12,1)); // Same scene, ranking output.
    assert(!p.admit(1,16,.5f));
    assert(p.admit(1,7,1));
    assert(!p.admit(2,16,.5f)); // Recreated owner cannot inherit a fade.
    assert(p.admit(2,7,1));
    assert(p.admit(2,15,1));
    assert(p.admit(2,16,0));
    assert(!p.admit(2,17,0));
    for(float invalid : {-1.f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        assert(!p.admit(2,7,invalid));
        assert(!p.admit(2,8,0));
    }
    assert(!p.admit(0,7,1));
    assert(p.admit(1,7,1));p.reset();assert(!p.admit(1,15,1));
}

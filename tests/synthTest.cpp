#include <gtest/gtest.h>

#include <math.h>
#include "audio.h"
#include "synth.h"

namespace{
    class SynthTest : public ::testing::Test
    {
    protected:
        virtual void SetUp()
        {
            AudioCtrl::Create();
            Synth::Create( 440.0 );
        }

        virtual void TearDown()
        {
            Synth::Destroy();
            AudioCtrl::Destroy();
        }
    };

    TEST_F(SynthTest, GetInstance){
        Synth* synth = Synth::GetInstance();
        EXPECT_NE( nullptr, synth );
    }
}
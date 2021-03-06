/**
 * @file synth.cpp
 */
#include <iostream>
#include <list>
#include <chrono>  // for high_resolution_clock

#include <math.h>

#include "common.h"
#include "waveform.h"

#include "audio.h"
#include "midi.h"
#include "synth.h"
#include "voice.h"

#include "screen_ui.h"


static double synth_signal_callback( void* userdata );

Synth* Synth::instance_ = nullptr;


/**
 * @brief Create
 */
Synth* Synth::Create( float tuning )
{
    if (!instance_)
    {
        instance_ = new Synth();
        instance_->Initialize( tuning );
    }
    return instance_;
}


/**
 * @brief Destroy
 */
void Synth::Destroy()
{
    AudioCtrl* audioctrl = AudioCtrl::GetInstance();
    audioctrl->SignalCallbackUnset();

    delete instance_->voicectrl_;

    delete instance_;
    instance_ = nullptr;
}

/**
 * @brief GetInstance
 */
Synth* Synth::GetInstance()
{
    return instance_;
}

/**
 * @brief initialize class
 */
void Synth::Initialize( float tuning )
{
    audioctrl_ = AudioCtrl::GetInstance();

    // create waveform
    Waveform* wf = Waveform::Create( tuning, audioctrl_->SampleRateGet() );

    // create Voice controller
    voicectrl_ = new VoiceCtrl();

    // set audio callback
    audioctrl_->SignalCallbackSet( synth_signal_callback, this );
}

/**
 * @brief Start synthesizer
 */
void Synth::Start()
{
    audioctrl_->Start();
}

///////////////////////////////////////////////////////////////////////////////

inline unsigned long long rdtsc() {
    unsigned long long ret;
    __asm__ volatile ("rdtsc" : "=A" (ret));
    return ret;
}

/**
 * @brief Signal callback
 */
float Synth::SignalCallback()
{
    unsigned long long start = rdtsc();

    MidiCtrl* midictrl = MidiCtrl::GetInstance();

    if( midictrl->IsStatusChanged() ) {
        voicectrl_->Trigger();            // トリガー/リリース処理
        midictrl->ResetStatusChange();  // 鍵盤状態変更フラグを落とす
    }

    // 各ボイスの信号処理を行いステレオMIXする（ボイスコントローラーの仕事）。
    float val = voicectrl_->SignalProcess();

    ScreenUI* screen_ui = ScreenUI::GetInstance();
    screen_ui->WaveformPut(val);

    unsigned long long stop = rdtsc();
    sigproc_time_ = stop - start;

    return val;
}

/**
 * @brief Signal callback handler
 */
static double synth_signal_callback( void* userdata )
{
    Synth* synth = (Synth*)userdata;
    return synth->SignalCallback();
}

///////////////////////////////////////////////////////////////////////////////

/**
 * @brief VoiceCtrl constructor
 */
VoiceCtrl::VoiceCtrl()
{
    key_mode_         = kPoly;
    current_voice_no_ = 0;
    unison_num_       = 1;
    poly_num_         = 16;  // 16 voices
    for(int ix=0; ix<kVoiceNum; ix++) {
        voice_[ix] = new Voice();
        voice_[ix]->SetNo(ix);
    }
}

#if 0
/**
 * @brief NoteOn
 */
void VoiceCtrl::NoteOn( int notenum, int velocity )
{
    for(int ix = 0; ix<kVoiceNum; ix++) {
        if( voice[ix]->key_on_ == false ) {
            Voice *v     = voice[ix];
            v->nn_       = notenum;
            v->velocity_ = velocity;
            v->key_on_   = true;
            //printf("key ON : nn=%d, v=%d\n", notenum, velocity);
            break;
        }
    }

    return;
}

/**
 * @brief NoteOff
 */
void VoiceCtrl::NoteOff( int notenum )
{
    for(int ix = 0; ix<kVoiceNum; ix++) {
        if( voice[ix]->nn_ == notenum ) {
            voice[ix]->key_on_ = false;
            //printf("key OFF: nn=%d, v=%d\n", notenum, velocity);
            break;
        }
    }

    return;
}
#endif

/**
 * @brief GetNextOffVoice
 */
Voice* VoiceCtrl::GetNextOffVoice()
{
    int current_voice_no = current_voice_no_;
    for( int i=0; i<kVoiceNum; i++ ) {
        current_voice_no = (current_voice_no + 1) % kVoiceNum;
        if( !voice_[current_voice_no]->IsKeyOn() ) {
            return voice_[current_voice_no];
        }
    }
    // どれもオンだった。
    return nullptr;
}

/**
 * @brief Trigger
 */
void VoiceCtrl::Trigger()
{
    if(key_mode_ == kPoly) TriggerPoly();
    else                   TriggerMono();

    return;
}


/**
 * @brief ポリモード時のトリガー/リリースを制御
 */
void VoiceCtrl::TriggerPoly()
{
    MidiCtrl* midictrl = MidiCtrl::GetInstance();

    //// まずキーリリース処理
    // オンボイスリストからみて、キーボードテーブル上でノートオフ(ベロシティ値=0)に
    // なっているかをチェック→ノートオフならリリースする
    for( std::list<Voice*>::iterator v=on_voices_.begin(); v != on_voices_.end(); ) {
        if( midictrl->GetVelocity( (*v)->GetNoteNo() ) == 0 ) {
            (*v)->Release();
            v = on_voices_.erase(v);
        }
        else {
            v++;
        }
    }

    //// トリガー処理
    // 新規に押鍵されたキーを対象に処理を行う。
    int num = MIN( poly_num_, midictrl->GetOnKeyNum() );    // 処理する最大ノート数はポリ数と押鍵キー数のどちらか少ない方
    for( int i=0; i<num; i++ ) {

        int noteNo = midictrl->GetNewOnKeyNN(i); // 新規押鍵キーでi番目に新しいキーのノートNO（NEWキーフラグ付き）
        if(noteNo == -1) break;                 // 新規押鍵キーはなかった→これ以上古い新規押鍵もないはずなので処理終了

        // 既に同じノートNoを発音しているボイスがあれば、リリースする。
        // 例えばアルペジエータでゲートタイム１００％とした時に、この対応がなければ、音がどんどん重なっていく。
        // これは、このアルゴリズムではnote on/offは鍵盤の状態を変更するだけで、もし同一時刻にoff→onの順で同時にきた場合、
        // 結果的にnoteoffは無視される事になるため。
        // このような事にならないために、同一ノートの複数ボイス発音は不可とする。
        for( std::list<Voice*>::iterator v=on_voices_.begin(); v != on_voices_.end(); ) {
            if((*v)->GetNoteNo() == noteNo) {
                (*v)->Release();
                v = on_voices_.erase(v);
            }
            else v++;
        }

        // ユニゾンボイスの数だけ、トリガー処理
        Voice* pUnisonMasterVoice;
        for(int u = 0; u < unison_num_; u++) {
            //// トリガーするボイスの決定
            // キーオフされているボイスを取得。もし全部ビジーだったら、一番古くからオンになっているボイスを取得
            // ※ただし、ベース音は除くなどの工夫の余地はある
            Voice* v = GetNextOffVoice();
            if(v == NULL){
                v = on_voices_.front();    // オン中で一番古いボイス
                if(v == NULL) return;
                on_voices_.pop_front();
            }
            current_voice_no_ = v->GetNo();
            if(u==0) pUnisonMasterVoice = v; // ユニソンマスターボイスの退避

            // ノートNO等の設定とトリガー
            v->SetNoteInfo(noteNo,midictrl->GetVelocity(noteNo));
            v->SetUnisonInfo(pUnisonMasterVoice,unison_num_,u);
            v->Trigger();

            // オンボイスリストへ追加
            on_voices_.push_back(v);
        }
    }
}

// モノモード時のトリガー/リリースを制御
void VoiceCtrl::TriggerMono()
{
    MidiCtrl* midictrl = MidiCtrl::GetInstance();

    // なにもキーがおさえられていなければ、現在のオンボイスをリリースして終わり
    if(midictrl->GetOnKeyNum()==0) {
        for(int i=0;i < unison_num_; i++) {
            on_voices_.remove(voice_[i]);
            if(voice_[i]->IsKeyOn()) voice_[i]->Release();
        }
        mono_current_velocity_ = 0;
        return;
    }

    // なにかキーがおさえられているので一番新しい鍵盤をとってきて、発音が必要か判定
    // 1.新規の押鍵なら無条件に発音
    // 2.新規ではない場合、ボイスがオン中でないか、オン中でもノートNOが違えば発音
    bool bProcess = false;
    int noteNo = midictrl->GetNewOnKeyNN(0);
    if(noteNo != -1){
        bProcess = true;
        mono_current_velocity_ = midictrl->GetVelocity(noteNo);
    }else{
        // 新規ではない→何かキーが離された結果、発音される事になったノート
        // このベロシティは離されたキーと同じとする。よって、m_monoCurrentVelocityは更新しない。
        noteNo = midictrl->GetOnKeyNN(0);
        if(!voice_[0]->IsKeyOn() || voice_[0]->GetNoteNo() != noteNo) bProcess = true;
    }

    if(!bProcess) return;    // 発音不要

    // 発音処理
    Voice* pUnisonMasterVoice = voice_[0];    // ユニゾンマスターは常にボイス番号ゼロ
    if(key_mode_ == kMono) {
        // モノモード時
        // 必ずトリガー
        for(int i=0;i < unison_num_; i++) {
            Voice* v = voice_[i];
            // ノートNO等の設定とトリガー
            v->SetNoteInfo(noteNo,mono_current_velocity_);
            v->SetUnisonInfo(pUnisonMasterVoice,unison_num_,0);
            v->Trigger();

            // オンボイスリストへ追加 (すでに登録されている可能性があるので、一度削除してから追加)
            on_voices_.remove(v);
            on_voices_.push_back(v);
        }
    }
    else if( key_mode_ == kLegato ) {
        // レガートモノモード時
        // 現在キーオフだった場合のみトリガー
        for(int i=0;i < unison_num_; i++) {
            Voice* v = voice_[i];
            // ノートNO等の設定とトリガー
            v->SetNoteInfo(noteNo,mono_current_velocity_);
            if(!v->IsKeyOn()){
                // 現在キーオフ→トリガーし、オンボイスリストへ追加
                v->SetUnisonInfo(pUnisonMasterVoice,unison_num_,0);
                v->Trigger();
                on_voices_.push_back(v);
            }else{
                // 現在オン→トリガーしない
            }
        }
    }
}


/**
 * @brief SignalProcess
 */
float VoiceCtrl::SignalProcess()
{
    double   val = 0;
    uint32_t w;
    for(int ix = 0; ix<kVoiceNum; ix++) {
        if( voice_[ix]->IsPlaying() == true ) {
            Voice *v = voice_[ix];
            val += v->Calc();
        }
    }

    return val;
}

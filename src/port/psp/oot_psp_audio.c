#include "attributes.h"
#include "audio.h"
#include "audiomgr.h"
#include "array_count.h"
#include "controller.h"
#include "ocarina.h"
#include "padmgr.h"
#include "save.h"
#include "seqcmd.h"
#include "sequence.h"
#include "sfx.h"

#include <string.h>

Vec3f gSfxDefaultPos = { 0.0f, 0.0f, 0.0f };
f32 gSfxDefaultFreqAndVolScale = 1.0f;
s8 gSfxDefaultReverb = 0;
u8 gMorphaTransposeTable[16] = { 0, 0, 0, 1, 1, 2, 4, 6, 8, 8, 8, 8, 8, 8, 8, 8 };

static u16 sPspActiveSeqIds[] = {
    NA_BGM_DISABLED,
    NA_BGM_DISABLED,
    NA_BGM_DISABLED,
    NA_BGM_DISABLED,
};
static u16 sPspPrevMainBgmSeqId = NA_BGM_DISABLED;
static s8 sPspFunc800F4524Reverb = 0;

static void OotPspAudio_ResetOcarina(void);
static void OotPspAudio_UpdateOcarina(void);

static OcarinaStaff sPspPlayingStaff;
static OcarinaStaff sPspPlaybackStaff;
static OcarinaStaff sPspRecordingStaff;
static u8 sPspFrogsSongNotes[14] = {
    OCARINA_BTN_A,       OCARINA_BTN_C_LEFT,  OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_LEFT,
    OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN,  OCARINA_BTN_A,       OCARINA_BTN_C_DOWN, OCARINA_BTN_A,
    OCARINA_BTN_C_DOWN,  OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT,  OCARINA_BTN_A,
};
static OcarinaNote sPspScarecrowLongSongNotes[108] = {
    { OCARINA_PITCH_NONE, 0, 0, 0, 0, 0 },
    { OCARINA_PITCH_NONE, 0, 0, 0, 0, 0 },
};
static u8 sPspScarecrowSpawnSong[0x80];

u8* gFrogsSongPtr = sPspFrogsSongNotes;
OcarinaNote* gScarecrowLongSongPtr = sPspScarecrowLongSongNotes;
u8* gScarecrowSpawnSongPtr = sPspScarecrowSpawnSong;

OcarinaSongButtons gOcarinaSongButtons[OCARINA_SONG_MAX] = {
    { 6, { OCARINA_BTN_A, OCARINA_BTN_C_UP, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_RIGHT } },
    { 8, { OCARINA_BTN_C_DOWN, OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_A, OCARINA_BTN_C_RIGHT,
           OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN } },
    { 5, { OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT } },
    { 6, { OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_A, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN,
           OCARINA_BTN_A } },
    { 7, { OCARINA_BTN_C_LEFT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_A, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN } },
    { 6, { OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_UP } },
    { 6, { OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT,
           OCARINA_BTN_C_LEFT } },
    { 6, { OCARINA_BTN_C_UP, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_UP, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_RIGHT } },
    { 6, { OCARINA_BTN_C_LEFT, OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_UP,
           OCARINA_BTN_C_RIGHT } },
    { 6, { OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN,
           OCARINA_BTN_C_UP } },
    { 6, { OCARINA_BTN_C_RIGHT, OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_A,
           OCARINA_BTN_C_DOWN } },
    { 6, { OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_UP, OCARINA_BTN_A, OCARINA_BTN_C_DOWN,
           OCARINA_BTN_C_UP } },
    { 8, { 0 } },
    { 0, { 0 } },
};

static void OotPspAudio_SetSeq(u8 seqPlayerIndex, u16 seqId) {
    if (seqPlayerIndex >= ARRAY_COUNT(sPspActiveSeqIds)) {
        return;
    }

    sPspActiveSeqIds[seqPlayerIndex] = seqId;
    if (seqPlayerIndex == SEQ_PLAYER_BGM_MAIN) {
        gSaveContext.seqId = seqId & 0xFF;
    }
}

void AudioMgr_Init(UNUSED AudioMgr* audioMgr, UNUSED void* stack, UNUSED OSPri pri, UNUSED OSId id,
                   UNUSED Scheduler* sched, UNUSED IrqMgr* irqMgr) {
}

void AudioMgr_WaitForInit(UNUSED AudioMgr* audioMgr) {
}

void AudioMgr_StopAllSfx(void) {
}

void Audio_Init(void) {
    Audio_ResetActiveSequences();
    OotPspAudio_ResetOcarina();
}

void Audio_InitSound(void) {
    Audio_Init();
}

void Audio_Update(void) {
    OotPspAudio_UpdateOcarina();
}

void Audio_PreNMI(void) {
    Audio_ResetActiveSequences();
}

void Audio_StartSequence(u8 seqPlayerIndex, u8 seqId, u8 seqArgs, UNUSED u16 fadeInDuration) {
    OotPspAudio_SetSeq(seqPlayerIndex, seqId | (seqArgs << 8));
}

void Audio_StopSequence(u8 seqPlayerIndex, UNUSED u16 fadeOutDuration) {
    OotPspAudio_SetSeq(seqPlayerIndex, NA_BGM_DISABLED);
}

void Audio_QueueSeqCmd(u32 cmd) {
    u8 op = (cmd >> 28) & 0xF;
    u8 seqPlayerIndex = (cmd >> 24) & 0xF;
    u8 fadeTimer = (cmd >> 16) & 0xFF;
    u8 seqArgs = (cmd >> 8) & 0xFF;
    u8 seqId = cmd & 0xFF;

    switch (op) {
        case SEQCMD_OP_PLAY_SEQUENCE:
        case SEQCMD_OP_QUEUE_SEQUENCE:
            Audio_StartSequence(seqPlayerIndex, seqId, seqArgs, fadeTimer);
            break;

        case SEQCMD_OP_STOP_SEQUENCE:
        case SEQCMD_OP_UNQUEUE_SEQUENCE:
            Audio_StopSequence(seqPlayerIndex, cmd & 0xFF);
            break;

        case SEQCMD_OP_GLOBAL_CMD:
            if (((cmd >> 8) & 0xF) == SEQCMD_SUB_OP_GLOBAL_SET_SOUND_OUTPUT_MODE) {
                Audio_SetSoundOutputMode(cmd & 0xFF);
            }
            break;

        default:
            break;
    }
}

void Audio_ProcessSeqCmds(void) {
}

u16 Audio_GetActiveSeqId(u8 seqPlayerIndex) {
    if (seqPlayerIndex >= ARRAY_COUNT(sPspActiveSeqIds)) {
        return NA_BGM_DISABLED;
    }

    return sPspActiveSeqIds[seqPlayerIndex];
}

s32 Audio_IsSeqCmdNotQueued(UNUSED u32 cmdVal, UNUSED u32 cmdMask) {
    return true;
}

void Audio_SetVolumeScale(UNUSED u8 seqPlayerIndex, UNUSED u8 scaleIndex, UNUSED u8 targetVol,
                          UNUSED u8 volFadeTimer) {
}

void Audio_UpdateActiveSequences(void) {
}

u8 func_800FAD34(void) {
    return 0;
}

void Audio_ResetActiveSequences(void) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sPspActiveSeqIds); i++) {
        sPspActiveSeqIds[i] = NA_BGM_DISABLED;
    }
}

void Audio_ResetActiveSequencesAndVolume(void) {
    Audio_ResetActiveSequences();
}

void Audio_SetSfxBanksMute(UNUSED u16 muteMask) {
}

void Audio_QueueSeqCmdMute(UNUSED u8 channelIndex) {
}

void Audio_ClearBGMMute(UNUSED u8 channelIndex) {
}

void Audio_PlaySfxGeneral(UNUSED u16 sfxId, UNUSED Vec3f* pos, UNUSED u8 token, UNUSED f32* freqScale,
                          UNUSED f32* vol, UNUSED s8* reverbAdd) {
}

void Audio_ProcessSfxRequest(void) {
}

void Audio_ChooseActiveSfx(UNUSED u8 bankId) {
}

void Audio_PlayActiveSfx(UNUSED u8 bankId) {
}

void Audio_StopSfxByBank(UNUSED u8 bankId) {
}

void Audio_RemoveSfxFromBankByPos(UNUSED u8 bankId, UNUSED Vec3f* pos) {
}

void Audio_StopSfxByPosAndBank(UNUSED u8 bankId, UNUSED Vec3f* pos) {
}

void Audio_StopSfxByPos(UNUSED Vec3f* pos) {
}

void Audio_StopSfxByPosAndId(UNUSED Vec3f* pos, UNUSED u16 sfxId) {
}

void Audio_StopSfxByTokenAndId(UNUSED u8 token, UNUSED u16 sfxId) {
}

void Audio_StopSfxById(UNUSED u32 sfxId) {
}

void Audio_ProcessSfxRequests(void) {
}

void func_800F8F88(void) {
}

u8 Audio_IsSfxPlaying(UNUSED u32 sfxId) {
    return false;
}

void Audio_ResetSfx(void) {
}

void Audio_SetSfxProperties(UNUSED u8 bankId, UNUSED u8 entryIdx, UNUSED u8 channelIndex) {
}

void Audio_PlayCutsceneEffectsSequence(UNUSED u8 csEffectType) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_SUB, NA_BGM_CUTSCENE_EFFECTS);
}

void func_800F4010(Vec3f* pos, u16 sfxId, UNUSED f32 arg2) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void Audio_PlaySfxRandom(Vec3f* pos, u16 baseSfxId, UNUSED u8 randLim) {
    Audio_PlaySfxGeneral(baseSfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void func_800F4138(Vec3f* pos, u16 sfxId, f32 arg2) {
    func_800F4010(pos, sfxId, arg2);
}

void func_800F4190(Vec3f* pos, u16 sfxId) {
    func_800F4010(pos, sfxId, 1.0f);
}

void func_800F436C(Vec3f* pos, u16 sfxId, UNUSED f32 arg2) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void func_800F4414(Vec3f* pos, u16 sfxId, f32 arg2) {
    func_800F436C(pos, sfxId, arg2);
}

void func_800F44EC(UNUSED s8 arg0, UNUSED s8 arg1) {
}

void func_800F4524(Vec3f* pos, u16 sfxId, s8 arg2) {
    sPspFunc800F4524Reverb = arg2;
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &sPspFunc800F4524Reverb);
}

void Audio_PlaySwordChargeSfx(Vec3f* pos, u8 level) {
    Audio_PlaySfxGeneral(NA_SE_IT_SWORD_CHARGE - 1 + level, pos, 4, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_PlaySfxRiver(Vec3f* pos, f32 freqScale) {
    Audio_PlaySfxGeneral(NA_SE_EV_RIVER_STREAM, pos, 4, &freqScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_PlaySfxWaterfall(Vec3f* pos, f32 freqScale) {
    Audio_PlaySfxGeneral(NA_SE_EV_WATER_WALL, pos, 4, &freqScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_SetBgmVolumeOffDuringFanfare(void) {
}

void Audio_SetBgmVolumeOnDuringFanfare(void) {
}

void Audio_SetMainBgmVolume(UNUSED u8 targetVol, UNUSED u8 volFadeTimer) {
}

void Audio_SetGanonsTowerBgmVolumeLevel(UNUSED u8 ganonsTowerLevel) {
}

void Audio_LowerMainBgmVolume(UNUSED u8 volume) {
}

void Audio_PlaySfxIncreasinglyTransposed(Vec3f* pos, s16 sfxId, UNUSED u8* semitones) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_ResetIncreasingTranspose(void) {
}

void Audio_PlaySfxTransposed(Vec3f* pos, u16 sfxId, UNUSED s8 semitone) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void func_800F4C58(Vec3f* pos, u16 sfxId, UNUSED u8 arg2) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void func_800F4E30(UNUSED Vec3f* pos, UNUSED f32 arg1) {
}

void Audio_ClearSariaBgm(void) {
    if (Audio_GetActiveSeqId(SEQ_PLAYER_BGM_SUB) == NA_BGM_SARIA_THEME) {
        Audio_StopSequence(SEQ_PLAYER_BGM_SUB, 0);
    }
}

void Audio_ClearSariaBgmAtPos(UNUSED Vec3f* pos) {
    Audio_ClearSariaBgm();
}

void Audio_PlaySariaBgm(UNUSED Vec3f* pos, u16 seqId, UNUSED u16 distMax) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_SUB, seqId);
}

void Audio_ClearSariaBgm2(void) {
    Audio_ClearSariaBgm();
}

void Audio_PlayMorningSceneSequence(u16 seqId) {
    Audio_PlaySceneSequence(seqId);
}

void Audio_PlaySceneSequence(u16 seqId) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_MAIN, seqId);
    gSaveContext.forcedSeqId = seqId;
}

void Audio_SetMainBgmTempoFreqAfterFanfare(UNUSED f32 scaleTempoAndFreq, UNUSED u8 duration) {
}

void Audio_PlayWindmillBgm(void) {
    Audio_PlaySceneSequence(NA_BGM_WINDMILL);
}

void Audio_SetFastTempoForTimedMinigame(void) {
}

void Audio_PlaySequenceInCutscene(u16 seqId) {
    Audio_PlaySceneSequence(seqId);
}

void Audio_StopSequenceInCutscene(u16 seqId) {
    if (Audio_IsSequencePlaying(seqId)) {
        Audio_StopSequence(SEQ_PLAYER_BGM_MAIN, 0);
        Audio_StopSequence(SEQ_PLAYER_FANFARE, 0);
    }
}

s32 Audio_IsSequencePlaying(u16 seqId) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sPspActiveSeqIds); i++) {
        if ((sPspActiveSeqIds[i] & 0xFF) == (seqId & 0xFF)) {
            return true;
        }
    }

    return false;
}

void func_800F5ACC(u16 seqId) {
    u16 curSeqId = Audio_GetActiveSeqId(SEQ_PLAYER_BGM_MAIN);

    if ((curSeqId != NA_BGM_DISABLED) && ((curSeqId & 0xFF) != (seqId & 0xFF))) {
        sPspPrevMainBgmSeqId = curSeqId;
    }

    Audio_PlaySceneSequence(seqId);
}

void func_800F5B58(void) {
    if (sPspPrevMainBgmSeqId != NA_BGM_DISABLED) {
        Audio_PlaySceneSequence(sPspPrevMainBgmSeqId);
        sPspPrevMainBgmSeqId = NA_BGM_DISABLED;
    }
}

void func_800F5BF0(u8 natureAmbienceId) {
    Audio_PlayNatureAmbienceSequence(natureAmbienceId);
}

void Audio_PlayFanfare(u16 seqId) {
    OotPspAudio_SetSeq(SEQ_PLAYER_FANFARE, seqId);
}

void func_800F5C2C(void) {
}

void Audio_PlaySequenceWithSeqPlayerIO(u8 seqPlayerIndex, u16 seqId, u8 fadeInDuration, UNUSED s8 ioPort,
                                       UNUSED s8 ioData) {
    Audio_StartSequence(seqPlayerIndex, seqId & 0xFF, (seqId >> 8) & 0xFF, fadeInDuration);
}

void Audio_SetSequenceMode(UNUSED u8 seqMode) {
}

void Audio_SetBgmEnemyVolume(UNUSED f32 dist) {
}

void Audio_UpdateMalonSinging(UNUSED f32 dist, u16 seqId) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_SUB, seqId);
}

void func_800F64E0(UNUSED u8 arg0) {
}

void Audio_ToggleMalonSinging(UNUSED u8 malonSingingDisabled) {
}

void Audio_SetEnvReverb(UNUSED s8 reverb) {
}

void Audio_SetCodeReverb(UNUSED s8 reverb) {
}

void Audio_SetSoundOutputMode(s8 soundSetting) {
    gSaveContext.soundSetting = soundSetting;
}

void Audio_SetBaseFilter(UNUSED u8 filter) {
}

void Audio_SetExtraFilter(UNUSED u8 filter) {
}

void Audio_SetCutsceneFlag(UNUSED s8 flag) {
}

void Audio_PlaySfxIfNotInCutscene(u16 sfxId) {
    SFX_PLAY_CENTERED(sfxId);
}

void func_800F6964(u16 fadeOutDuration) {
    Audio_StopBgmAndFanfare(fadeOutDuration);
}

void Audio_StopBgmAndFanfare(u16 fadeOutDuration) {
    Audio_StopSequence(SEQ_PLAYER_BGM_MAIN, fadeOutDuration);
    Audio_StopSequence(SEQ_PLAYER_FANFARE, fadeOutDuration);
}

void func_800F6B3C(void) {
}

void func_800F6BDC(void) {
}

void Audio_SetNatureAmbienceChannelIO(UNUSED u8 channelIdxRange, UNUSED u8 ioPort, UNUSED u8 ioData) {
}

void Audio_PlayNatureAmbienceSequence(u8 natureAmbienceId) {
    gSaveContext.natureAmbienceId = natureAmbienceId;
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_MAIN, NA_BGM_NATURE_AMBIENCE);
}

void func_800F7170(void) {
}

void func_800F71BC(UNUSED s32 arg0) {
}

#define OOT_PSP_OCARINA_ALLOWED_BUTTONS (BTN_A | BTN_CUP | BTN_CDOWN | BTN_CLEFT | BTN_CRIGHT)
#define OOT_PSP_OCARINA_SONG_FLAG_MASK  0x3FFF
#define OOT_PSP_OCARINA_MODE_MASK       0xC000
#define OOT_PSP_OCARINA_MODE_NO_STAFF   0x4000
#define OOT_PSP_OCARINA_MODE_PLAYBACK   0x8000
#define OOT_PSP_OCARINA_MODE_FREE_PLAY  0xC000
#define OOT_PSP_OCARINA_STATE_PLAYING   0xFE
#define OOT_PSP_OCARINA_STATE_OFF       0xFF
#define OOT_PSP_OCARINA_NOTE_FRAMES     12

static const u8 sPspButtonToPitchMap[5] = {
    OCARINA_PITCH_D4, // OCARINA_BTN_A
    OCARINA_PITCH_F4, // OCARINA_BTN_C_DOWN
    OCARINA_PITCH_A4, // OCARINA_BTN_C_RIGHT
    OCARINA_PITCH_B4, // OCARINA_BTN_C_LEFT
    OCARINA_PITCH_D5, // OCARINA_BTN_C_UP
};

static const u8 sPspPitchToButtonMap[16] = {
    OCARINA_BTN_A,                 // OCARINA_PITCH_C4
    OCARINA_BTN_A,                 // OCARINA_PITCH_DFLAT4
    OCARINA_BTN_A,                 // OCARINA_PITCH_D4
    OCARINA_BTN_A,                 // OCARINA_PITCH_EFLAT4
    OCARINA_BTN_C_DOWN,            // OCARINA_PITCH_E4
    OCARINA_BTN_C_DOWN,            // OCARINA_PITCH_F4
    OCARINA_BTN_C_DOWN,            // OCARINA_PITCH_GFLAT4
    OCARINA_BTN_C_RIGHT,           // OCARINA_PITCH_G4
    OCARINA_BTN_C_RIGHT,           // OCARINA_PITCH_AFLAT4
    OCARINA_BTN_C_RIGHT,           // OCARINA_PITCH_A4
    OCARINA_BTN_C_RIGHT_OR_C_LEFT, // OCARINA_PITCH_BFLAT4
    OCARINA_BTN_C_LEFT,            // OCARINA_PITCH_B4
    OCARINA_BTN_C_LEFT,            // OCARINA_PITCH_C5
    OCARINA_BTN_C_UP,              // OCARINA_PITCH_DFLAT5
    OCARINA_BTN_C_UP,              // OCARINA_PITCH_D5
    OCARINA_BTN_C_UP,              // OCARINA_PITCH_EFLAT5
};

static u8 sPspOcarinaInstrumentId = OCARINA_INSTRUMENT_OFF;
static u32 sPspOcarinaFlags;
static u16 sPspAvailOcarinaSongFlags;
static u8 sPspFirstOcarinaSongIndex;
static u8 sPspLastOcarinaSongIndex;
static u8 sPspOcarinaInputEnabled;
static u8 sPspStaffOcarinaPlayingPos;
static u8 sPspOcarinaInputButtonStart;
static u16 sPspOcarinaInputButtonCur;
static u8 sPspSongInputLen;
static u8 sPspSongInputBuf[8];
static u8 sPspPlaybackButtons[ARRAY_COUNT(sPspScarecrowLongSongNotes)];
static u8 sPspPlaybackLen;
static u8 sPspPlaybackPos;
static u8 sPspPlaybackStaffPos;
static u8 sPspPlaybackState;
static u8 sPspPlaybackTimer;
static u8 sPspRecordingState = OCARINA_RECORD_OFF;
static u8 sPspRecordButtons[8];
static u8 sPspRecordButtonCount;
static u8 sPspLongRecordNoteCount;
static u8 sPspMemoryGameButtons[8];
static u8 sPspMemoryGameLen;
static u8 sPspMemoryGameEndLen;
static u32 sPspOcarinaRandom = 1;

static void OotPspAudio_ResetOcarina(void) {
    sPspOcarinaInstrumentId = OCARINA_INSTRUMENT_OFF;
    sPspOcarinaFlags = 0;
    sPspAvailOcarinaSongFlags = 0;
    sPspFirstOcarinaSongIndex = 0;
    sPspLastOcarinaSongIndex = OCARINA_SONG_MEMORY_GAME;
    sPspOcarinaInputEnabled = false;
    sPspStaffOcarinaPlayingPos = 0;
    sPspOcarinaInputButtonStart = 0;
    sPspOcarinaInputButtonCur = 0;
    sPspSongInputLen = 0;
    memset(sPspSongInputBuf, OCARINA_BTN_INVALID, sizeof(sPspSongInputBuf));

    sPspPlaybackLen = 0;
    sPspPlaybackPos = 0;
    sPspPlaybackStaffPos = 0;
    sPspPlaybackState = 0;
    sPspPlaybackTimer = 0;
    memset(sPspPlaybackButtons, OCARINA_BTN_INVALID, sizeof(sPspPlaybackButtons));

    sPspRecordingState = OCARINA_RECORD_OFF;
    sPspRecordButtonCount = 0;
    sPspLongRecordNoteCount = 0;
    memset(sPspRecordButtons, OCARINA_BTN_INVALID, sizeof(sPspRecordButtons));

    sPspMemoryGameLen = 0;
    sPspMemoryGameEndLen = 0;
    sPspOcarinaRandom = 1;

    sPspPlayingStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspPlayingStaff.state = OOT_PSP_OCARINA_STATE_OFF;
    sPspPlayingStaff.pos = 0;
    sPspPlaybackStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspPlaybackStaff.state = 0;
    sPspPlaybackStaff.pos = 0;
    sPspRecordingStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspRecordingStaff.state = OCARINA_RECORD_OFF;
    sPspRecordingStaff.pos = 0;
}

static u16 OotPspAudio_OcarinaMode(void) {
    return (u16)sPspOcarinaFlags & OOT_PSP_OCARINA_MODE_MASK;
}

static u8 OotPspAudio_MapPitchToButton(u8 pitchAndBFlatFlag) {
    u8 buttonIndex;

    if ((pitchAndBFlatFlag & 0x3F) >= ARRAY_COUNT(sPspPitchToButtonMap)) {
        return OCARINA_BTN_INVALID;
    }

    buttonIndex = sPspPitchToButtonMap[pitchAndBFlatFlag & 0x3F];
    if (buttonIndex == OCARINA_BTN_C_RIGHT_OR_C_LEFT) {
        return (pitchAndBFlatFlag & 0x80) ? OCARINA_BTN_C_RIGHT : OCARINA_BTN_C_LEFT;
    }

    return buttonIndex;
}

static u8 OotPspAudio_MapButtonToPitch(u8 buttonIndex) {
    if (buttonIndex >= ARRAY_COUNT(sPspButtonToPitchMap)) {
        return OCARINA_PITCH_NONE;
    }

    return sPspButtonToPitchMap[buttonIndex];
}

static u8 OotPspAudio_GetSongLength(u8 songIndex) {
    if (songIndex == OCARINA_SONG_MEMORY_GAME) {
        return sPspMemoryGameLen;
    }

    if (songIndex == OCARINA_SONG_SCARECROW_SPAWN) {
        return 8;
    }

    if (songIndex < OCARINA_SONG_MAX) {
        return gOcarinaSongButtons[songIndex].numButtons;
    }

    return 0;
}

static u8 OotPspAudio_GetSongButton(u8 songIndex, u8 buttonPos) {
    if (songIndex == OCARINA_SONG_MEMORY_GAME) {
        return (buttonPos < sPspMemoryGameLen) ? sPspMemoryGameButtons[buttonPos] : OCARINA_BTN_INVALID;
    }

    if (songIndex == OCARINA_SONG_SCARECROW_SPAWN) {
        return (buttonPos < 8) ? sPspScarecrowSpawnSong[buttonPos] : OCARINA_BTN_INVALID;
    }

    if (songIndex < OCARINA_SONG_MAX && buttonPos < gOcarinaSongButtons[songIndex].numButtons) {
        return gOcarinaSongButtons[songIndex].buttonsIndex[buttonPos];
    }

    return OCARINA_BTN_INVALID;
}

static s32 OotPspAudio_MatchesSongAt(u8 songIndex, const u8* buttons, u8 buttonCount, u8 offset) {
    u8 i;

    for (i = 0; i < buttonCount; i++) {
        if (buttons[i] != OotPspAudio_GetSongButton(songIndex, offset + i)) {
            return false;
        }
    }

    return true;
}

static s32 OotPspAudio_IsSongPrefix(u8 songIndex, const u8* buttons, u8 buttonCount) {
    u8 songLength = OotPspAudio_GetSongLength(songIndex);

    return (buttonCount <= songLength) && OotPspAudio_MatchesSongAt(songIndex, buttons, buttonCount, 0);
}

static s32 OotPspAudio_RecentButtonsMatchSong(u8 songIndex) {
    u8 songLength = OotPspAudio_GetSongLength(songIndex);

    if (songLength == 0 || sPspSongInputLen < songLength) {
        return false;
    }

    return OotPspAudio_MatchesSongAt(songIndex, &sPspSongInputBuf[sPspSongInputLen - songLength], songLength, 0);
}

static u8 OotPspAudio_ButtonFromInput(u16 buttons) {
    if (CHECK_BTN_ANY(buttons, BTN_A)) {
        return OCARINA_BTN_A;
    }
    if (CHECK_BTN_ANY(buttons, BTN_CDOWN)) {
        return OCARINA_BTN_C_DOWN;
    }
    if (CHECK_BTN_ANY(buttons, BTN_CRIGHT)) {
        return OCARINA_BTN_C_RIGHT;
    }
    if (CHECK_BTN_ANY(buttons, BTN_CLEFT)) {
        return OCARINA_BTN_C_LEFT;
    }
    if (CHECK_BTN_ANY(buttons, BTN_CUP)) {
        return OCARINA_BTN_C_UP;
    }

    return OCARINA_BTN_INVALID;
}

static void OotPspAudio_AppendInputButton(u8 buttonIndex) {
    u16 mode = OotPspAudio_OcarinaMode();

    if (mode == OOT_PSP_OCARINA_MODE_FREE_PLAY || mode == OOT_PSP_OCARINA_MODE_NO_STAFF) {
        sPspStaffOcarinaPlayingPos++;
        if (sPspStaffOcarinaPlayingPos > 8) {
            sPspStaffOcarinaPlayingPos = 1;
        }

        if (sPspSongInputLen == ARRAY_COUNT(sPspSongInputBuf)) {
            memmove(&sPspSongInputBuf[0], &sPspSongInputBuf[1], ARRAY_COUNT(sPspSongInputBuf) - 1);
        } else {
            sPspSongInputLen++;
        }
        sPspSongInputBuf[sPspSongInputLen - 1] = buttonIndex;
    } else {
        sPspStaffOcarinaPlayingPos++;
        if (sPspSongInputLen < ARRAY_COUNT(sPspSongInputBuf)) {
            sPspSongInputBuf[sPspSongInputLen++] = buttonIndex;
        }
    }

    sPspPlayingStaff.buttonIndex = buttonIndex;
    sPspPlayingStaff.pos = sPspStaffOcarinaPlayingPos;
}

static void OotPspAudio_CheckSongs(void) {
    u16 mode = OotPspAudio_OcarinaMode();
    s32 completeSong = -1;
    s32 anyPrefix = false;
    u8 songIndex;

    if (sPspOcarinaFlags == 0) {
        return;
    }

    if (mode == OOT_PSP_OCARINA_MODE_FREE_PLAY || mode == OOT_PSP_OCARINA_MODE_NO_STAFF) {
        for (songIndex = sPspFirstOcarinaSongIndex; songIndex < sPspLastOcarinaSongIndex; songIndex++) {
            if ((sPspAvailOcarinaSongFlags & (1 << songIndex)) && OotPspAudio_RecentButtonsMatchSong(songIndex)) {
                completeSong = songIndex;
                break;
            }
        }
    } else {
        for (songIndex = sPspFirstOcarinaSongIndex; songIndex < sPspLastOcarinaSongIndex; songIndex++) {
            if (!(sPspAvailOcarinaSongFlags & (1 << songIndex))) {
                continue;
            }

            if (OotPspAudio_IsSongPrefix(songIndex, sPspSongInputBuf, sPspSongInputLen)) {
                anyPrefix = true;
                if (sPspSongInputLen == OotPspAudio_GetSongLength(songIndex)) {
                    completeSong = songIndex;
                    break;
                }
            }
        }
    }

    if (completeSong >= 0) {
        sPspPlayingStaff.state = completeSong;
        sPspOcarinaInputEnabled = false;
        sPspOcarinaFlags = 0;
    } else if ((mode == OOT_PSP_OCARINA_MODE_PLAYBACK && !anyPrefix) ||
               (mode != OOT_PSP_OCARINA_MODE_FREE_PLAY && mode != OOT_PSP_OCARINA_MODE_NO_STAFF &&
                sPspSongInputLen >= ARRAY_COUNT(sPspSongInputBuf))) {
        sPspPlayingStaff.state = OOT_PSP_OCARINA_STATE_OFF;
        sPspOcarinaInputEnabled = false;
        sPspOcarinaFlags = 0;
    }
}

static s32 OotPspAudio_IsReservedScarecrowSpawnSong(void) {
    u8 songIndex;
    u8 start;
    u8 i;
    s32 allSame = true;

    for (i = 1; i < ARRAY_COUNT(sPspRecordButtons); i++) {
        if (sPspRecordButtons[i] != sPspRecordButtons[0]) {
            allSame = false;
            break;
        }
    }

    if (allSame) {
        return true;
    }

    for (songIndex = 0; songIndex < OCARINA_SONG_SCARECROW_SPAWN; songIndex++) {
        u8 songLength = OotPspAudio_GetSongLength(songIndex);

        if (songLength == 0 || songLength > ARRAY_COUNT(sPspRecordButtons)) {
            continue;
        }

        for (start = 0; start <= ARRAY_COUNT(sPspRecordButtons) - songLength; start++) {
            if (OotPspAudio_MatchesSongAt(songIndex, &sPspRecordButtons[start], songLength, 0)) {
                return true;
            }
        }
    }

    return false;
}

static void OotPspAudio_AcceptScarecrowSpawnSong(void) {
    u8 i;

    memset(sPspScarecrowSpawnSong, 0, sizeof(sPspScarecrowSpawnSong));
    gOcarinaSongButtons[OCARINA_SONG_SCARECROW_SPAWN].numButtons = 8;

    for (i = 0; i < ARRAY_COUNT(sPspRecordButtons); i++) {
        sPspScarecrowSpawnSong[i] = sPspRecordButtons[i];
        gOcarinaSongButtons[OCARINA_SONG_SCARECROW_SPAWN].buttonsIndex[i] = sPspRecordButtons[i];
    }
}

static void OotPspAudio_RecordButton(u8 buttonIndex) {
    OcarinaNote* note;

    if (sPspRecordingState == OCARINA_RECORD_OFF) {
        return;
    }

    sPspRecordingStaff.buttonIndex = buttonIndex;
    sPspRecordingStaff.pos = ++sPspStaffOcarinaPlayingPos;

    if (sPspRecordingState == OCARINA_RECORD_SCARECROW_SPAWN) {
        if (sPspRecordButtonCount < ARRAY_COUNT(sPspRecordButtons)) {
            sPspRecordButtons[sPspRecordButtonCount++] = buttonIndex;
        }

        if (sPspRecordButtonCount == ARRAY_COUNT(sPspRecordButtons)) {
            if (OotPspAudio_IsReservedScarecrowSpawnSong()) {
                sPspRecordingState = OCARINA_RECORD_REJECTED;
            } else {
                OotPspAudio_AcceptScarecrowSpawnSong();
                sPspRecordingState = OCARINA_RECORD_OFF;
            }
            sPspOcarinaInputEnabled = false;
        }
    } else if (sPspLongRecordNoteCount < ARRAY_COUNT(sPspScarecrowLongSongNotes) - 1) {
        note = &sPspScarecrowLongSongNotes[sPspLongRecordNoteCount++];
        note->pitch = OotPspAudio_MapButtonToPitch(buttonIndex);
        note->length = OOT_PSP_OCARINA_NOTE_FRAMES;
        note->volume = 0x57;
        note->vibrato = 0;
        note->bend = 0;
        note->bFlat4Flag = 0;

        sPspScarecrowLongSongNotes[sPspLongRecordNoteCount].pitch = OCARINA_PITCH_NONE;
        sPspScarecrowLongSongNotes[sPspLongRecordNoteCount].length = 0;
    }
}

static void OotPspAudio_LoadPlaybackSong(s8 songIndex) {
    u8 i;

    sPspPlaybackLen = 0;

    if (songIndex == OCARINA_SONG_SCARECROW_LONG) {
        for (i = 0; i < ARRAY_COUNT(sPspScarecrowLongSongNotes) && sPspPlaybackLen < ARRAY_COUNT(sPspPlaybackButtons);
             i++) {
            OcarinaNote* note = &gScarecrowLongSongPtr[i];

            if (note->length == 0) {
                break;
            }
            if (note->pitch != OCARINA_PITCH_NONE) {
                sPspPlaybackButtons[sPspPlaybackLen++] =
                    OotPspAudio_MapPitchToButton(note->pitch | note->bFlat4Flag);
            }
        }
        return;
    }

    if (songIndex < 0 || songIndex >= OCARINA_SONG_MAX) {
        return;
    }

    for (i = 0; i < OotPspAudio_GetSongLength(songIndex) && sPspPlaybackLen < ARRAY_COUNT(sPspPlaybackButtons); i++) {
        sPspPlaybackButtons[sPspPlaybackLen++] = OotPspAudio_GetSongButton(songIndex, i);
    }
}

static void OotPspAudio_UpdatePlayback(void) {
    u8 buttonIndex;

    if (sPspPlaybackState == 0) {
        return;
    }

    if (sPspPlaybackTimer != 0) {
        sPspPlaybackTimer--;
        return;
    }

    if (sPspPlaybackPos >= sPspPlaybackLen) {
        sPspPlaybackState = 0;
        sPspPlaybackStaff.state = 0;
        sPspPlaybackStaff.pos = 0;
        sPspPlaybackStaff.buttonIndex = OCARINA_BTN_INVALID;
        return;
    }

    buttonIndex = sPspPlaybackButtons[sPspPlaybackPos++];
    if (buttonIndex == OCARINA_BTN_INVALID) {
        return;
    }

    sPspPlaybackStaffPos++;
    if (sPspPlaybackStaffPos > 8) {
        sPspPlaybackStaffPos = 1;
    }

    sPspPlaybackStaff.buttonIndex = buttonIndex;
    sPspPlaybackStaff.state = sPspPlaybackState;
    sPspPlaybackStaff.pos = sPspPlaybackStaffPos;
    sPspPlaybackTimer = OOT_PSP_OCARINA_NOTE_FRAMES;
}

static void OotPspAudio_UpdateOcarinaInput(void) {
    const Input* input = &gPadMgr.inputs[0];
    u16 prevButtons = sPspOcarinaInputButtonCur;
    u16 curButtons = input->cur.button;
    u16 noteButtons;
    u8 buttonIndex;

    sPspOcarinaInputButtonCur = curButtons;
    noteButtons = curButtons & OOT_PSP_OCARINA_ALLOWED_BUTTONS;

    if (sPspOcarinaInputButtonStart != 0) {
        if (noteButtons == sPspOcarinaInputButtonStart) {
            return;
        }
        sPspOcarinaInputButtonStart = 0;
    }

    buttonIndex = OotPspAudio_ButtonFromInput((curButtons ^ prevButtons) & curButtons &
                                             OOT_PSP_OCARINA_ALLOWED_BUTTONS);
    if (buttonIndex == OCARINA_BTN_INVALID) {
        return;
    }

    OotPspAudio_AppendInputButton(buttonIndex);

    if (sPspRecordingState != OCARINA_RECORD_OFF) {
        OotPspAudio_RecordButton(buttonIndex);
    } else {
        OotPspAudio_CheckSongs();
    }
}

static void OotPspAudio_UpdateOcarina(void) {
    OotPspAudio_UpdatePlayback();

    if (sPspOcarinaInstrumentId != OCARINA_INSTRUMENT_OFF && sPspOcarinaInputEnabled) {
        OotPspAudio_UpdateOcarinaInput();
    }

    sPspRecordingStaff.state = sPspRecordingState;
    if (sPspOcarinaFlags != 0 && sPspPlayingStaff.state == OOT_PSP_OCARINA_STATE_OFF) {
        sPspPlayingStaff.state = OOT_PSP_OCARINA_STATE_PLAYING;
    }
}

void AudioOcarina_Start(u16 ocarinaFlags) {
    u8 i;

    if (ocarinaFlags == 0xFFFF) {
        sPspOcarinaFlags = 0;
        sPspOcarinaInputEnabled = false;
        sPspPlayingStaff.buttonIndex = OCARINA_BTN_INVALID;
        sPspPlayingStaff.state = OOT_PSP_OCARINA_STATE_OFF;
        sPspPlayingStaff.pos = 0;
        return;
    }

    if (gSaveContext.save.info.scarecrowSpawnSongSet && ((ocarinaFlags & 0xFFF) == 0xFFF)) {
        ocarinaFlags |= (1 << OCARINA_SONG_SCARECROW_SPAWN);
    }

    sPspOcarinaFlags = 0x80000000 | ocarinaFlags;
    sPspAvailOcarinaSongFlags = ocarinaFlags & OOT_PSP_OCARINA_SONG_FLAG_MASK;
    sPspFirstOcarinaSongIndex = 0;
    sPspLastOcarinaSongIndex = (ocarinaFlags == 0xA000) ? OCARINA_SONG_MAX : OCARINA_SONG_MEMORY_GAME;
    sPspOcarinaInputEnabled = true;
    sPspStaffOcarinaPlayingPos = 0;
    sPspSongInputLen = 0;

    for (i = 0; i < ARRAY_COUNT(sPspSongInputBuf); i++) {
        sPspSongInputBuf[i] = OCARINA_BTN_INVALID;
    }

    sPspPlayingStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspPlayingStaff.state = OOT_PSP_OCARINA_STATE_PLAYING;
    sPspPlayingStaff.pos = 0;
}

void AudioOcarina_SetInstrument(u8 ocarinaInstrumentId) {
    sPspOcarinaInstrumentId = ocarinaInstrumentId;

    if (ocarinaInstrumentId == OCARINA_INSTRUMENT_OFF) {
        sPspOcarinaInputEnabled = false;
        sPspOcarinaFlags = 0;
        sPspPlaybackState = 0;
        sPspPlaybackStaff.state = 0;
        sPspPlayingStaff.buttonIndex = OCARINA_BTN_INVALID;
        sPspPlayingStaff.state = OOT_PSP_OCARINA_STATE_OFF;
        sPspPlayingStaff.pos = 0;
    } else {
        sPspOcarinaInputButtonCur = gPadMgr.inputs[0].cur.button;
        sPspOcarinaInputButtonStart = sPspOcarinaInputButtonCur & OOT_PSP_OCARINA_ALLOWED_BUTTONS;
    }
}

void AudioOcarina_SetPlaybackSong(s8 songIndexPlusOne, s8 playbackState) {
    if (songIndexPlusOne == 0) {
        sPspPlaybackState = 0;
        sPspPlaybackStaff.buttonIndex = OCARINA_BTN_INVALID;
        sPspPlaybackStaff.state = 0;
        sPspPlaybackStaff.pos = 0;
        return;
    }

    OotPspAudio_LoadPlaybackSong(songIndexPlusOne - 1);
    sPspPlaybackState = playbackState;
    sPspPlaybackPos = 0;
    sPspPlaybackStaffPos = 0;
    sPspPlaybackTimer = 0;
    sPspPlaybackStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspPlaybackStaff.state = playbackState;
    sPspPlaybackStaff.pos = 0;
}

void AudioOcarina_SetRecordingState(u8 recordingState) {
    if (recordingState == sPspRecordingState) {
        return;
    }

    if (recordingState != OCARINA_RECORD_OFF) {
        sPspRecordingState = recordingState;
        sPspOcarinaInputEnabled = true;
        sPspStaffOcarinaPlayingPos = 0;
        sPspRecordButtonCount = 0;
        sPspLongRecordNoteCount = 0;
        memset(sPspRecordButtons, OCARINA_BTN_INVALID, sizeof(sPspRecordButtons));
        sPspRecordingStaff.buttonIndex = OCARINA_BTN_INVALID;
        sPspRecordingStaff.state = recordingState;
        sPspRecordingStaff.pos = 0;
    } else {
        if (sPspRecordingState == OCARINA_RECORD_SCARECROW_LONG &&
            sPspLongRecordNoteCount < ARRAY_COUNT(sPspScarecrowLongSongNotes)) {
            sPspScarecrowLongSongNotes[sPspLongRecordNoteCount].pitch = OCARINA_PITCH_NONE;
            sPspScarecrowLongSongNotes[sPspLongRecordNoteCount].length = 0;
        }

        sPspRecordingState = OCARINA_RECORD_OFF;
        sPspOcarinaInputEnabled = false;
        sPspRecordingStaff.state = OCARINA_RECORD_OFF;
        sPspRecordingStaff.pos = 0;
    }
}

OcarinaStaff* AudioOcarina_GetRecordingStaff(void) {
    return &sPspRecordingStaff;
}

OcarinaStaff* AudioOcarina_GetPlayingStaff(void) {
    return &sPspPlayingStaff;
}

OcarinaStaff* AudioOcarina_GetPlaybackStaff(void) {
    return &sPspPlaybackStaff;
}

void AudioOcarina_MemoryGameInit(u8 minigameRound) {
    static u8 sMemoryGameNumNotes[] = { 5, 6, 8 };
    u8 i;

    if (minigameRound >= ARRAY_COUNT(sMemoryGameNumNotes)) {
        minigameRound = ARRAY_COUNT(sMemoryGameNumNotes) - 1;
    }

    sPspMemoryGameLen = 0;
    sPspMemoryGameEndLen = sMemoryGameNumNotes[minigameRound];

    for (i = 0; i < 3; i++) {
        AudioOcarina_MemoryGameNextNote();
    }
}

s32 AudioOcarina_MemoryGameNextNote(void) {
    u8 randomButtonIndex;

    if (sPspMemoryGameLen >= sPspMemoryGameEndLen || sPspMemoryGameLen >= ARRAY_COUNT(sPspMemoryGameButtons)) {
        return 1;
    }

    sPspOcarinaRandom = (sPspOcarinaRandom * 1103515245) + 12345;
    randomButtonIndex = (sPspOcarinaRandom >> 16) % 5;
    if (sPspMemoryGameLen != 0 && sPspMemoryGameButtons[sPspMemoryGameLen - 1] == randomButtonIndex) {
        randomButtonIndex = (randomButtonIndex + 1) % 5;
    }

    sPspMemoryGameButtons[sPspMemoryGameLen++] = randomButtonIndex;
    return 0;
}

void AudioOcarina_PlayLongScarecrowSong(void) {
    AudioOcarina_SetPlaybackSong(OCARINA_SONG_SCARECROW_LONG + 1, 1);
}

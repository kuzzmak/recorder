#include <QtTest>

#include "core/AudioMixer.h"

class TstAudiomixer : public QObject
{
    Q_OBJECT

private slots:
    void mix_sumsTwoStreams();
    void mix_appliesGains();
    void mix_hardClipsToInt16Range();
    void mix_nullSecondStream_passesFirstThrough();
    void mix_inPlace_dstAliasesA();
    void peak_isLargestAbsoluteSample();
};

void TstAudiomixer::mix_sumsTwoStreams()
{
    const int16_t a[] = {100, -200, 300, 0};
    const int16_t b[] = {1, 2, 3, 4};
    int16_t dst[4] = {};
    AudioMixer::mix(dst, a, b, 4);
    QCOMPARE(dst[0], int16_t(101));
    QCOMPARE(dst[1], int16_t(-198));
    QCOMPARE(dst[2], int16_t(303));
    QCOMPARE(dst[3], int16_t(4));
}

void TstAudiomixer::mix_appliesGains()
{
    const int16_t a[] = {1000, -1000};
    const int16_t b[] = {200, 200};
    int16_t dst[2] = {};
    AudioMixer::mix(dst, a, b, 2, 0.5, 2.0);
    QCOMPARE(dst[0], int16_t(900));
    QCOMPARE(dst[1], int16_t(-100));
}

void TstAudiomixer::mix_hardClipsToInt16Range()
{
    const int16_t a[] = {30000, -30000, 32767};
    const int16_t b[] = {30000, -30000, 0};
    int16_t dst[3] = {};
    AudioMixer::mix(dst, a, b, 3, 1.0, 1.0);
    QCOMPARE(dst[0], int16_t(32767));
    QCOMPARE(dst[1], int16_t(-32768));
    // Gain alone can push a single stream past full scale too.
    AudioMixer::mix(dst, a, b, 3, 2.0, 0.0);
    QCOMPARE(dst[2], int16_t(32767));
    QCOMPARE(dst[1], int16_t(-32768));
}

void TstAudiomixer::mix_nullSecondStream_passesFirstThrough()
{
    const int16_t a[] = {10, -20, 30};
    int16_t dst[3] = {};
    AudioMixer::mix(dst, a, nullptr, 3, 0.5, 7.0);
    QCOMPARE(dst[0], int16_t(5));
    QCOMPARE(dst[1], int16_t(-10));
    QCOMPARE(dst[2], int16_t(15));
}

void TstAudiomixer::mix_inPlace_dstAliasesA()
{
    int16_t a[] = {100, 200, 300};
    const int16_t b[] = {1, 1, 1};
    AudioMixer::mix(a, a, b, 3);
    QCOMPARE(a[0], int16_t(101));
    QCOMPARE(a[1], int16_t(201));
    QCOMPARE(a[2], int16_t(301));
}

void TstAudiomixer::peak_isLargestAbsoluteSample()
{
    const int16_t pcm[] = {100, -2500, 2400, 0};
    QCOMPARE(AudioMixer::peak(pcm, 4), 2500);
    const int16_t full[] = {-32768, 5};
    QCOMPARE(AudioMixer::peak(full, 2), 32768);
    QCOMPARE(AudioMixer::peak(pcm, 0), 0);
    QCOMPARE(AudioMixer::peak(nullptr, 0), 0);
}

QTEST_GUILESS_MAIN(TstAudiomixer)
#include "tst_audiomixer.moc"

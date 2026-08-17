#include "corpus.h"

#include <algorithm>

namespace {

// A tiny deterministic PRNG rather than <random>. std::mt19937 would work
// equally well; what matters is that the sequence is fixed by the seed and
// identical on every machine in the fleet, which the standard distributions do
// not guarantee across implementations.
struct lcg {
    uint64_t s;
    explicit lcg(uint32_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint32_t next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(s >> 33);
    }
    size_t pick(size_t n) { return next() % n; }
    // Percent roll, 0..99.
    uint32_t roll() { return next() % 100; }
};

// Reactions and acknowledgements. In real logs this class dominates by count
// and contributes almost nothing by volume.
const char* const k_tiny[] = {
    "ㅋㅋ", "ㅋㅋㅋ", "ㅋㅋㅋㅋㅋ", "ㅇㅇ", "ㄴㄴ", "네", "넵", "ㅇㅋ", "ㄱㄱ",
    "아", "음", "헐", "ㄷㄷ", "오", "와", "ㅠㅠ", "ㅜㅜ", "?", "??", "!!",
    "굿", "ok", "ㅇㅈ", "ㄹㅇ", "인정", "감사", "ㄳ", "ㅎㅇ", "ㅂㅂ", "고고",
};

const char* const k_short[] = {
    "어디야?", "지금 뭐해?", "밥 먹었어?", "언제 와?", "다 왔어?", "몇 시야?",
    "이거 맞아?", "확인했어?", "지금 가?", "끝났어?", "괜찮아?", "가능해?",
    "나 도착", "먼저 가 있어", "조금 늦어", "5분만", "거의 다 옴", "출발함",
    "잠깐만", "다시 보내줘", "안 보여", "잘 들려?", "소리가 안 나",
    "링크 좀", "파일 받았어", "지금 확인 중", "내일 다시 얘기하자",
};

const char* const k_mid[] = {
    "아 그거 내일까지인데 아직 시작도 못 했어",
    "회의 끝나고 바로 연락할게 지금은 좀 어려워",
    "그 파일 어디 있는지 아는 사람 있어? 어제 올린 것 같은데",
    "지금 서버가 좀 이상한데 다들 접속 되나요",
    "아까 얘기한 부분은 일단 그대로 두고 다음 주에 다시 보자",
    "점심 뭐 먹을지 정해서 알려주세요 오늘은 좀 일찍 나가려고요",
    "그 부분은 제가 처리해 두겠습니다 확인만 부탁드려요",
    "테스트 돌려봤는데 결과가 좀 이상하게 나와서 다시 확인 중입니다",
    "주말에 시간 되면 잠깐 보자 오래 안 걸릴 거야",
    "방금 올린 버전으로 다시 받아주세요 이전 건 문제가 있었습니다",
    "생각보다 오래 걸리네요 조금만 더 기다려 주시면 감사하겠습니다",
    "저는 그 의견에 동의하는데 다른 분들은 어떻게 생각하시는지 궁금합니다",
};

// The paragraph tail. Rare, but it is the class that crosses the MSS and takes
// a different path through the stack, so it has to exist.
const char* const k_long[] = {
    "일단 상황을 정리해 보면 어제 배포한 버전에서 문제가 발생한 건 맞고, "
    "다만 원인이 이번 변경 때문인지 아니면 그 전부터 있던 건데 이제 드러난 "
    "건지가 아직 불분명합니다. 로그를 다시 보고 있는데 재현이 안 되는 게 "
    "제일 곤란한 부분이에요. 혹시 비슷한 증상 겪으신 분 계시면 알려주세요.",

    "제가 이해한 게 맞다면 지금 논의되는 건 두 가지가 섞여 있는 것 같습니다. "
    "하나는 당장 이번 주에 처리해야 하는 문제고, 다른 하나는 구조를 어떻게 "
    "가져갈 거냐는 더 큰 이야기인데, 이 둘을 같이 결정하려고 하니까 계속 "
    "결론이 안 나는 것 같아요. 급한 것부터 먼저 막고 나머지는 따로 시간을 "
    "잡아서 얘기하는 게 어떨까요.",

    "주말 동안 생각해 봤는데 처음 방향이 틀렸던 것 같습니다. 성능이 문제라고 "
    "생각해서 계속 그쪽만 봤는데, 실제로 사용자가 불편해하는 건 느린 게 "
    "아니라 가끔 응답이 아예 안 오는 거였어요. 평균은 멀쩡한데 꼬리가 "
    "긴 상황이라서, 평균만 보고 있으면 영원히 못 찾을 문제였습니다. "
    "다음 주에 측정 방식부터 다시 잡아보려고 합니다.",
};

template <size_t N>
const char* pick_from(lcg& r, const char* const (&arr)[N])
{
    return arr[r.pick(N)];
}

}  // namespace

void corpus::build(uint32_t seed, size_t entries, size_t max_bytes)
{
    lcg r(seed);
    lines_.clear();
    lines_.reserve(entries);
    total_ = 0;
    min_ = static_cast<size_t>(-1);
    max_ = 0;

    for (size_t i = 0; i < entries; ++i) {
        const uint32_t k = r.roll();
        std::string s;

        if (k < 45) {
            // Reactions, sometimes doubled the way people actually type them.
            s = pick_from(r, k_tiny);
            if (r.roll() < 25) { s += " "; s += pick_from(r, k_tiny); }
        } else if (k < 75) {
            s = pick_from(r, k_short);
        } else if (k < 94) {
            s = pick_from(r, k_mid);
        } else if (k < 99) {
            // Two sentences run together — the "I have one more thing" shape.
            s = pick_from(r, k_mid);
            s += " ";
            s += pick_from(r, k_mid);
        } else {
            s = pick_from(r, k_long);
        }

        // Truncation is on a byte budget, so cut back to a UTF-8 boundary
        // rather than splitting a character. A split character would be a
        // malformed payload, which is a different experiment than this one.
        if (s.size() > max_bytes) {
            size_t cut = max_bytes;
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
                --cut;
            s.resize(cut);
        }

        total_ += s.size();
        min_ = std::min(min_, s.size());
        max_ = std::max(max_, s.size());
        lines_.push_back(std::move(s));
    }
    if (lines_.empty()) min_ = 0;
}

# openbfd

BFD3 콘텐츠 라이브러리를 읽어 연주하는 오픈소스 드럼 VI (JUCE / VST3).
`.bfdlac` 오디오와 킷·프리셋 XML을 직접 읽는다. BFD 라이브러리는 포함하지 않는다.

## 기능

- **코덱**: BFDLAC 디코더 (16종 블록 알고리즘, 16/24-bit)
- **킷 / 프리셋**: `.bfd2kit` `.bfd3kit` `.bfd2` `.bfd3` 읽기·쓰기.
  모델링하지 않는 섹션은 그대로 보존한다.
- **엔진**: 벨로시티 레이어, `Alt/` 라운드로빈, 백그라운드 프리로드, 디스크 스트리밍.
  오디오 스레드에서는 락·할당·파일 I/O를 하지 않는다.
- **믹서**: 킷의 모든 마이크(direct, ambient, other bleed)를 32개 출력 채널의
  L/R 잭에 패치한다. L만 꽂으면 모노, L+R은 스테레오, Mid+Side는 M/S이다.
  채널 N은 출력 버스 `Ch N`과 Main으로 나간다.
- **bleed**: 킥·스네어 bleed (Off / Dir / On), send / return / input trim, 마스터 K/S/O
- **키맵**: 프리셋의 MIDI 맵을 쓰고, 맵이 없으면 GM 자동 맵을 쓴다.

## UI

- **브라우저** (좌): 킷 · 프리셋 · 피스 목록과 검색.
  `+ FOLDER`로 라이브러리를 추가한다(기본값은 BFD3 `DataPaths.xml`).
- **스테이지** (중앙): 킷을 위에서 본 도면.
  - 피스는 브라우저에서 끌어다 놓아 배치하고, 클릭해 들어 보고, 드래그로 옮긴다.
  - 우클릭 메뉴로 위치를 초기화하거나 피스를 뺀다.
- **믹서** (우): 앞면에서는 팬, 페이더, 미터, M/S를 조절한다.
  뒷면에는 행마다 L/R 입력 잭과 bleed 설정이 있다.
- **케이블**:
  - 마이크 아이콘을 끌면 케이블이 나오고 믹서가 뒷면으로 돈다. 잭에 놓으면 꽂히고,
    다른 곳에 놓으면 케이블이 마이크로 감겨 들어간다.
  - 뒷면에서는 꽂힌 플러그를 다른 잭으로 옮길 수 있다.
  - `HIDE CABLES`로 케이블을 감추고, `REAR`로 뒷면을 고정한다.
- 라이트 / 다크 테마

## 빌드

CMake 3.22+, C++20 컴파일러, [JUCE](https://github.com/juce-framework/JUCE) 9가 필요하다.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DJUCE_DIR=/path/to/JUCE
cmake --build build -j8
```

VST3는 `build/OpenBfd_artefacts/Release/VST3/openbfd.vst3`에 생성된다.

## 도구

| 타깃 | 용도 |
|---|---|
| `openbfd-tests` | 단위 테스트. `OPENBFD_LIBRARY=<라이브러리 경로>`를 주면 실제 라이브러리 검증도 한다 |
| `openbfd-decode` | `stats <file.bfdlac>` / `wav <file.bfdlac> <out.wav>` |
| `openbfd-render` | 실제 프로세서로 킷을 WAV로 렌더 |
| `openbfd-ui-shot` | DAW 없이 에디터 스크린샷: `<out.png> [light\|dark] [킷] [front\|rear\|cable\|bare] [WxH]` |

`tools/ref_bfdlac.py`는 파이썬 레퍼런스 디코더다.

## 구조

| 경로 | 내용 |
|---|---|
| `Source/Bfdlac/` | BFDLAC 코덱 |
| `Source/Model/` | 라이브러리·킷·프리셋 모델과 파서 |
| `Source/Engine/` | 보이스, 샘플 캐시, 스트리밍, 믹서 |
| `Source/Plugin/` | JUCE 프로세서와 에디터 (`UI/`: 브라우저, 스테이지, 믹서, 케이블) |
| `tools/`, `Tests/` | CLI 도구와 테스트 |

`Source/Bfdlac`, `Source/Model`, `Source/Engine`은 JUCE 없이 빌드된다(`openbfd_core`).

## 고지

BFD는 inMusic Brands의 상표이며, 이 프로젝트는 inMusic과 관계가 없다.
번들한 JetBrains Mono 글꼴은 SIL Open Font License 1.1을 따른다(`Resources/Fonts/OFL.txt`).

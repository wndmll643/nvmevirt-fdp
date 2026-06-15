# LSM 실험 핸드오프 (RocksDB식 워크로드 + RUH 수 sweep)

> **TL;DR** — 기존 `./run_sweep.sh`가 돌던 환경에서 **`./run_lsm.sh` 한 번** 돌리고,
> 생성되는 **`lsm_results.csv`** 파일만 보내주면 됩니다. 코드/하니스/논문은 이미 다
> 준비돼 있고, 실데이터만 채우면 끝납니다.

---

## 1. 왜 하는가 (목적)

현재 논문 평가의 약점 두 가지를 한 번에 보완하는 실험입니다.

1. **"합성 워크로드뿐"이라는 한계** — 지금은 인위적 hot/cold 2-클래스만 씀.
2. **RUH를 2개만 사용** — 장치는 RUH(reclaim unit handle)를 8개 노출하는데 워크로드는
   hot→RUH1, cold→RUH2 둘만 씀.

**아이디어:** LSM-tree(레벨드 컴팩션, RocksDB의 구조)는 **데이터 수명이 "레벨"과 직결**
됩니다. 상위 레벨(L0)은 작고 자주 덮어써져 hot, 하위 레벨은 크고 거의 안 바뀌어 cold,
인접 레벨은 약 `T`배(fanout)씩 갱신빈도가 차이남. 그래서 **"레벨 = 수명 클래스 = RUH"**.
이건 FDP의 교과서적 use case(RocksDB-on-FDP의 "레벨마다 reclaim unit 하나")입니다.

**측정:** 레벨들을 **몇 개의 RUH로 나누느냐(N=1..레벨수)**에 따라 WAF가 얼마나 줄어드는지.
→ (a) 현실적 워크로드에서도 FDP 효과 재현 + (b) "RUH가 몇 개 필요한가" 라는 새 결과.

---

## 2. 이미 준비된 것 (건드릴 필요 없음)

| 파일 | 역할 |
|------|------|
| `lsm_workload.c` | LSM 워크로드 생성기+리플레이어 (외부 RocksDB 불필요, 자체 생성) |
| `fdp_lsm.sh` | 게스트(VM) 안에서 도는 sweep 스크립트 |
| `run_lsm.sh` | 호스트에서 모듈 빌드 + VM 부팅 + 실행 |
| `paper/make_figs.py` | 결과 → `fig_lsm_ruh.pdf` |
| `docs/data/lsm_summary.csv` | **현재는 임시(placeholder) 숫자** — 실데이터로 교체 예정 |
| `paper/paper_ieee.tex` | LSM 소절 "A RocksDB-like LSM workload" 이미 통합됨 |
| `docs/results.md` §3.5 | 방법론 설명 |

기존 합성 실험(`fdp_workload.c`, `fdp_sweep.sh` 등)은 **전혀 안 건드렸습니다.**

---

## 3. 실험 실행 (팀원이 할 일)

### 3.1 환경 요구사항 ⚠️ 중요
- **Linux 6.x 커널** 권장. (원 실험 커널 = **6.12.77**, `docs/results.md` 참고.)
  - ⚠️ **RHEL 8의 4.18 커널에서는 안 됩니다.** 게스트에서 `modprobe nvme`가 커널을
    완전히 hang시킵니다 (확인됨). 즉 **기존 `./run_sweep.sh`가 돌던 그 머신/환경**이면 OK.
- `vng`(virtme-ng), QEMU, `nvme-cli`, `gcc`, `make`.

### 3.2 실행
```bash
cd nvmevirt-fdp
./run_lsm.sh                 # 기본: fanout T=4, levels L=4, churn 30 MiB, N=1..4 sweep
# 정상상태에 더 근접시키려면 churn을 키움(느려짐):
CHURN_MB=50 ./run_lsm.sh
```

- 호스트에서 `nvmev.ko` 빌드 → TCG VM 부팅 → 게스트에서 워크로드 실행 → WAF 수집.
- 수십 분 걸립니다 (sweep 5회 × fill+churn).
- 로그: `/tmp/fdp_lsm.log`.

### 3.3 산출물 → **이것만 보내주면 됩니다**
```
lsm_results.csv
```
스키마: `fanout,levels,num_ruh,seed,mode,dHBMW,dMBMW,MBE,WAF`
(host가 게스트 stdout의 `ROW,` 줄들로 자동 재구성함.)

### 3.4 결과 sanity 체크 (제대로 됐는지)
- `base`(baseline) WAF: N과 무관하게 거의 일정해야 함.
- `fdp` WAF: **N이 커질수록 감소**, **N=1 ≈ baseline**(단일 reclaim unit이라 효과 없음).
- N=4(=레벨 수)에서 감소폭 최대. 곡선에 "무릎(knee)" — 첫 분리(가장 hot한 레벨 격리)에서
  대부분의 이득이 나오고 이후 체감 둔화.
- 혹시 WAF가 전부 ≈1.0이면 GC가 거의 안 돈 것 → `CHURN_MB`를 키워 재실행.

---

## 4. (선택) 더 큰 sweep — RUH 최대 8까지

레벨 수를 늘리면 RUH sweep 범위가 넓어집니다. 단, 작은 장치(64MiB)에선 상위 레벨 밴드가
너무 작아지니 장치를 키웁니다 (예약 메모리 안에서 가능):
```bash
LSM_L=5 LSM_MEMSIZE=256M ./run_lsm.sh     # 레벨 5개 → N=1..5
```
워크로드가 "level X has only N IOs (< 8)" 에러를 내면 `LSM_T`를 줄이거나(예 `LSM_T=3`)
`LSM_MEMSIZE`를 키우세요.

---

## 5. 결과 받은 뒤 처리 (참고 — 내(요청자)가 함)

1. `lsm_results.csv` → `docs/data/lsm_summary.csv` 집계
   (스키마: `num_ruh,base_waf,fdp_waf,reduction_pct,endurance_x`, mode별 WAF 평균).
2. `cd paper && python3 make_figs.py` → `fig_lsm_ruh.pdf` 자동 갱신.
3. 논문 재빌드(`pdflatex paper_ieee && bibtex paper_ieee && pdflatex x2`) + 소절 수치 반영.

---

## 6. 파라미터 레퍼런스

**`lsm_workload <dev> <mode> <churn_MB> [T] [L] [N] [seed] [ws_pct]`**
| 인자 | 의미 | 기본 |
|------|------|------|
| `mode` | `fillbase`/`fillfdp`(채우기) 또는 `base`/`fdp`(churn) | — |
| `churn_MB` | churn 단계 쓰기량 (fill 단계는 0) | — |
| `T` | fanout(레벨 크기 비). 레벨 i 밴드 ∝ `T^i` | 4 |
| `L` | 레벨 수 (≤8) | 4 |
| `N` | 레벨을 담을 RUH 수 (N=1=전부 한 RU≈baseline, N=L=레벨마다 하나) | L |
| `seed` | 난수 시드 | 1 |
| `ws_pct` | 워킹셋 크기(장치 %) | 90 |

**`run_lsm.sh` / `fdp_lsm.sh` 환경변수**
| 변수 | 의미 | 기본 |
|------|------|------|
| `CHURN_MB` | churn 쓰기량 | 30 |
| `LSM_T` `LSM_L` `LSM_WS` | fanout / 레벨수 / 워킹셋% | 4 / 4 / 90 |
| `LSM_MEMSIZE` | 장치 크기 (insmod memmap_size) | 64M |
| `KERNEL` | 부팅할 vmlinuz 경로 | 호스트 커널 |
| `VNG_EXTRA` | vng 추가 플래그 (예 `--disable-microvm`) | (없음) |

---

## 7. 트러블슈팅

| 증상 | 원인 / 해결 |
|------|------|
| `modprobe nvme`에서 멈춤 / VM 무응답 | 커널이 너무 옛버전(4.18 등). **6.x 커널 사용** (`KERNEL=/path/to/vmlinuz-6.x`). |
| `vng: command not found` | virtme-ng 미설치/PATH. `pip install --user virtme-ng` 또는 기존 환경 활성화. |
| `'virtio-9p-pci' is not a valid device model` | QEMU에 9p 없음 → **virtiofs** 사용 (Rust `virtiofsd` 필요, §8). |
| `unsupported machine type` | QEMU가 microvm 미지원 → `VNG_EXTRA="--disable-microvm" ./run_lsm.sh`. |
| `statically linked busybox could not be found` | static busybox를 PATH에 두기 (§8). |
| WAF ≈ 1.0 전부 | GC 거의 안 돔 → `CHURN_MB` 키우기. |

---

## 8. (부록) 만약 팀원 환경에도 도구가 없다면 — **sudo 없이** 셋업 레시피

요청자 머신(RHEL 8)에서 sudo 없이 아래까지 실제로 구성/부팅 검증 완료함. (단 그 머신은
커널이 4.18이라 nvme가 hang해서 데이터만 못 뽑음.) 6.x 커널이 있는 환경이면 이대로 됨:

```bash
# 1) virtme-ng (사용자 설치)
python3 -m pip install --user virtme-ng        # ~/.local/bin/vng

# 2) QEMU — RHEL은 /usr/libexec/qemu-kvm 에 있음. PATH용 이름으로 링크
ln -sf /usr/libexec/qemu-kvm ~/.local/bin/qemu-system-x86_64

# 3) static busybox (정적 링크 필수)
#    배포판 busybox-static 패키지, 또는 busybox.net 공식 정적 바이너리를 ~/.local/bin/busybox 로.

# 4) (RHEL qemu는 9p가 없어 virtiofs 필요) Rust virtiofsd — unprivileged 지원
#    런타임 lib의 .so 링크가 없으면 빌드 링크 실패하므로 먼저 링크:
mkdir -p ~/.local/lib
ln -sf $(ls /usr/lib64/libseccomp.so.* | head -1) ~/.local/lib/libseccomp.so
ln -sf $(ls /usr/lib64/libcap-ng.so.* | head -1) ~/.local/lib/libcap-ng.so
RUSTFLAGS="-L $HOME/.local/lib" cargo install virtiofsd   # ~/.cargo/bin/virtiofsd

# 5) 실행 시 PATH/플래그
export PATH="$HOME/.cargo/bin:$HOME/.local/bin:$PATH"
KERNEL=/path/to/vmlinuz-6.x VNG_EXTRA="--disable-microvm" ./run_lsm.sh
```
- `/dev/kvm`가 누구나 접근 가능(0666)하면 KVM도 sudo 없이 쓸 수 있어 훨씬 빠름.
  (단 WAF는 timing-independent라 TCG/KVM 결과 동일.)
- 핵심 교훈: **VM 안에서는 자동 root**라 `insmod`/`nvme`에 호스트 sudo가 필요 없음.
  유일한 외부 의존성은 QEMU·virtme-ng·busybox·(virtiofsd) 바이너리뿐이고 전부 사용자
  공간에 둘 수 있음.

---

## 9. 한 줄 다시

> **기존 `./run_sweep.sh` 돌던 환경(=6.x 커널)에서 `./run_lsm.sh` → `lsm_results.csv` 전달.** 끝.

# Reed

**Um Wurlitzer 200A, reconstruído em ponto fixo para o M-VAVE FM-1.**

Reed é um projeto de firmware customizado, aberto, para o M-VAVE FM-1. O som é um
modelo físico portado do [OpenWurli](https://github.com/hal0zer0/openwurli):
sete modos de palheta, captação eletrostática resolvida por campo, martelo e
amortecedor, e o caráter do amplificador do 200A. Sem samples, sem respostas ao
impulso.

O motor é C99 portátil, **sem ponto flutuante, sem libc e sem alocação**. O mesmo
código roda no computador, no navegador (WebAssembly) e no JieLi AC79 do FM-1
(pi32v2, 360 MHz, sem FPU). O firmware usa a camada de hardware
[felucca-bsp](https://github.com/hugelton/felucca-bsp), extraída do
[Felucca](https://github.com/hugelton/Felucca).

| | |
|---|---|
| Motor | **v0.1, funcionando** — voz em ponto fixo + cadeia comportamental |
| Emulador | **funcionando** — o motor real em WebAssembly num AudioWorklet |
| Firmware FM-1 | **alfa** — a imagem compila (84 KB), mas não foi validada no hardware |
| Instalador web | **planejado** — nenhuma imagem será publicada antes da validação |
| Licença | GPL-3.0-only |

**Online:** [ograndebe.github.io/fm1-reed](https://ograndebe.github.io/fm1-reed/) ·
[emulador](https://ograndebe.github.io/fm1-reed/try/) ·
[código](https://github.com/ograndebe/fm1-reed)

## Testar no emulador

```sh
./web/build.sh
python3 -m http.server -d web 8080
# abra http://localhost:8080/try/
```

Clique em **Power** e toque as 27 teclas (F3–G5). Teclado do computador:
`A W S E D F T G Y H U J K`; oitava `Z/X`; sustain `Espaço`. Um teclado MIDI
conectado é usado automaticamente (Web MIDI, Chrome/Edge). Os cinco sliders são
os mesmos parâmetros do firmware: volume, tremolo, drive, speaker e afinação.

## Compilar e testar

```sh
make test          # testes do motor
make reed          # renderizador offline
./build/reed demo --out demo.wav
./build/reed midi --in musica.mid --out musica.wav
./build/reed bench
```

## Firmware

```sh
git submodule update --init --recursive
./firmware/build.sh
```

Precisa do toolchain JieLi (Linux x86-64) e, no macOS/arm64, Docker. A imagem
`firmware/build/reed.bin` é um alfa de bring-up: áudio ALNK0, 27 teclas, 7
encoders, LCD, MIDI USB e boot guard. **Não tem entrada de atualização/OTA, não
foi testada num FM-1 e não é uma release instalável.** Leia
[firmware/README.md](firmware/README.md) e [BUILDING.md](BUILDING.md) antes de
qualquer tentativa.

## Documentação

A documentação canônica é em inglês, em `docs/` e no site:

- [docs/PLAN.md](docs/PLAN.md) — plano por fases e critérios de aceitação
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — arquitetura, formatos Q, memória
- [docs/PORTING.md](docs/PORTING.md) — o que é port fiel, o que é comportamental,
  calibração e limitações
- [docs/HARDWARE.md](docs/HARDWARE.md) — fatos do FM-1, áudio, interrupções
- [BUILDING.md](BUILDING.md) — build, emulador, firmware e recuperação
- [web/docs.html](web/docs.html) — a mesma documentação no site

## Licença e créditos

GPL-3.0-only. As tabelas geradas derivam do OpenWurli (GPL-3.0-or-later); a
camada de hardware é MPL-2.0; a fonte 5×7 é BSD-3-Clause (Adafruit). Veja
[LICENSING.md](LICENSING.md) e [CREDITS.md](CREDITS.md).

M-VAVE e FM-1 são marcas de seus respectivos donos. Reed é não oficial e não é
afiliado nem endossado pela M-VAVE, JieLi, Hügelton Instruments ou pelos autores
do OpenWurli.

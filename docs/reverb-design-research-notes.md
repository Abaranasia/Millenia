# Reverb Design Research Notes — Valhalla DSP Articles

Notes on three articles by Sean Costello (Valhalla DSP), kept here as reference material
and a bibliography — not a proposal to change our architecture. Where an article suggests
something genuinely actionable for our Dattorro-based shimmer reverb that isn't already
in [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md), it's called out explicitly
under [Actionable ideas not yet in our docs](#actionable-ideas-not-yet-in-our-docs) —
everything else is just "worth having read."

**Sources:**
- Part 2: [Getting Started With Reverb Design, Part 2: The Best Papers](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/) (the article's own headline is "The Best Papers"; the URL slug says "the-foundations" — same page)
- Part 3: [Getting Started With Reverb Design, Part 3: Online Resources](https://valhalladsp.com/2021/09/23/getting-started-with-reverb-design-part-3-online-resources/)
- [Eno/Lanois Shimmer Sound: How it is made](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/) (2010) — the origin story of the shimmer effect specifically, not general reverb theory
- There's presumably a Part 1 in the "Getting Started" series (introductory) that wasn't given to research here — flag if it turns out to matter. The Eno/Lanois post also promises a follow-up analyzing feedback behavior of each component that wasn't tracked down here.

## Part 2 — the 10 papers Costello calls "foundations of all algorithmic digital reverbs that followed"

| # | Paper | Author(s) | Venue/Year | Core idea |
|---|---|---|---|---|
| 1 | "Colorless" Artificial Reverberation | Manfred Schroeder | JAES, 1961 | Introduces the **allpass delay line** — feedforward+feedback around a digital delay, unity gain at all frequencies. Cascading them raises echo density without added coloration. First published digital reverb concept. |
| 2 | Natural Sounding Artificial Reverberation | Manfred Schroeder | JAES, July 1962 | The "**Schroeder reverb**": parallel comb filters (feedback controls decay length) summed together, then through two short series allpasses to raise echo density. Also shows transposing series/parallel order and matrixing comb outputs for decorrelated multichannel output. |
| 3 | Synthetic Stereo Reverberation (Part 1) | Michael Gerzon | Studio Sound, Dec 1971, pp.632–635 | Establishes **feedback delay networks (FDN)**: parallel delays combined through a unitary (orthogonal) matrix, fed back into the delay inputs. Echo density *grows over time* (closer to a real room) instead of staying flat like Schroeder's design. Shows building higher-order unitary matrices from 2×2 rotation matrices, and using the rotation angle to control L/R reverb spread. |
| 4 | Synthetic Studio Reverberation (Part 2) | Michael Gerzon | Studio Sound, Jan 1972, pp.24–28 | Extends FDNs to **allpass feedback delay networks** and cascaded unitary networks; adds filters inside the unitary network for frequency-dependent decay time (matching real rooms), and shows how to replace an allpass delay's feedforward/feedback multiplier with a *filter* while keeping the whole thing allpass. Costello's pull quote: "By repeated applications of feedback networks and of cascading, a wide variety of unitary networks can be created out of just a few basic unitary circuits." |
| 5 | About This Reverberation Business | James Moorer | Computer Music Journal, June 1979 | Popularized the **2-multiply allpass** (more efficient than Schroeder's 3-multiply version) as a standard building block. Proposes lowpass filters *inside* comb filters for high-frequency damping (closer to real-room absorption), and a sparse FIR tapped delay line for early reflections, separate from the comb/allpass late-reverb network. |
| 6 | Designing Multi-Channel Reverberators | John Stautner & Miller Puckette | Computer Music Journal, Spring 1982 | Independently rediscovers FDNs (Gerzon's Studio Sound paper was hard to get in the US) for a wider audience, with published Music 11 source code. First public paper to use **slow random delay-length modulation** to improve reverb sound quality — a technique commercial units (EMT-250, 1976) already used but hadn't published. |
| 7 | A New Approach to Digital Reverberation using Closed Waveguide Networks | Julius O. Smith | ICMC 1985, Vancouver, pp.47–53 | Introduces **waveguide (bi-directional delay line) reverberators** and the "waveguide junction" as a scattering matrix — later concepts fed back into FDN design, plus modulation techniques. |
| 8 | Digital delay networks for designing artificial reverberators | Jean-Marc Jot & Antoine Chaigne | AES 90th Convention, Paris, Feb 1991 | Every delay line in an FDN gets **its own damping filter**, tuned so *every* line decays at the same rate at every frequency — this is specifically what suppresses metallic/uneven-eigenmode resonances in the tail. |
| 9 | A Realtime Multichannel Room Simulator | Bill Gardner | 124th ASA meeting, New Orleans, Nov 1992 | First **public** description of the "**allpass loop**" reverb family: several allpasses in series, embedded in one larger feedback loop, loop feedback gain sets decay time. Gives concrete small/medium/large-room allpass-loop examples and covers **nested allpass delays** (an allpass inside another allpass's feedback path). Also a practical reference for implementing all of this from one shared circular buffer. |
| 10 | Effect Design, Part 1: Reverberator and Other Filters | Jon Dattorro | JAES 45(9), Sept 1997 | Fully publishes a complete allpass-loop reverb, "in the style of [Lexicon's] Griesinger" — **this is our tank's namesake and topology**, with published delay lengths and coefficients. Includes commentary from Barry Blesser (EMT-250 designer) on *why* delay-line modulation was originally added to commercial reverbs. Already cited in [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md). |

Costello frames #1–#2 (Schroeder) and #9–#10 (Gardner/Dattorro) as two related but distinct
lineages: **Schroeder's parallel-comb design** vs. the **"allpass loop"** family (series
allpasses in one big feedback loop) that Gardner named and Dattorro fully documented — our
tank is squarely in the second lineage, not the first.

## Part 3 — where Costello says to keep researching

- **[Spin Semiconductor](http://www.spinsemi.com) / [FV-1 chip](http://www.experimentalnoize.com/product_FV-1.php)** — designed by Keith Barr (MXR, Alesis) and Frank Thomson; powers a large share of boutique reverb/delay pedals ([history](https://reverb.com/news/fv-1-chip-history-5-pedals)). The site's ["informal notes"](http://www.spinsemi.com/knowledge_base.html) are, per Costello, one of the best plain-language walkthroughs of allpass-loop reverbs available, and its [example algorithms](http://www.spinsemi.com/programs.php) are worth reading even without targeting the chip itself.
- **[Theory and Techniques of Electronic Music](http://msp.ucsd.edu/techniques.htm)** by Miller Puckette (co-author of paper #6 above) — free online textbook; explains FDNs and goes deep on **rotation matrices** and their equivalence to allpass delays, plus a worked example combining a feedforward unitary network (early reflections) with an FDN (late reverb). Companion [Pure Data](https://puredata.info/) patches explore more FDN variants.
- **[Physical Audio Signal Processing](https://ccrma.stanford.edu/~jos/pasp/)** by Julius O. Smith (paper #7 author) — already cited in `shimmer-reverb-concepts.md`'s References; Part 3 flags it again specifically for its treatment of comb/allpass delays, FDNs, waveguide reverbs, and the equivalences between all three.
- **[DAFx (Digital Audio Effects) conference](https://www.dafx.de)**, running since 1998 — [paper archive](https://www.dafx.de/paper-archive/search.php?years=1998) is, per Costello, "a treasure trove of modern reverb research" (some archive links are dead; worth persistence when searching).
- **[AES E-Library](https://www.aes.org/e-lib/)** — paywalled, but tens of thousands of papers back to the 1950s.
- Journals to search if going through a university library: *Journal of the Acoustical Society of America*, *IEEE*, *Computer Music Journal* — Costello's own workaround is walking into a public research university library with a USB stick.

## The shimmer effect's origin story (Eno/Lanois)

Per [Costello](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/),
the Brian Eno/Daniel Lanois "shimmer" sound is fundamentally: **a feedback loop containing
a +1 octave pitch shifter and a long-decay reverb**, with the loop's overall character
shaped by controlling feedback gain, EQ, and the delay lengths inside the loop — "the same
technique used by ValhallaShimmer, with the reverberation, pitch shifting and feedback all
incorporated within the same plugin." This is, structurally, exactly our architecture
(Dattorro tank + dual-delay-line shifter at the feedback junction + feedback gain parameter)
— useful as historical confirmation that the recipe we picked is the canonical one, not a
simplification of it.

**The actual hardware chain**, per Kevin Killen's account of U2's "4th of July," quoted in
the [Eno/Lanois post](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/)
from a [Gearslutz thread](http://www.gearslutz.com/board/1112956-post2.html): an **AMS DMX
15-80S** (digital delay/sampler/pitch shifter, common in early-1980s Britain) fed into a
**Lexicon 224** reverb using its "Concert Hall" algorithm (~5s decay, EQ'd), with sends
driven "almost recirculating out of control" on an analog mixer to build up a "layer upon
layer" effect. Other accounts substitute an **EMT-250** for the reverb, or a **Lexicon Prime
Time** modulated delay line instead of/alongside the reverb. The common thread across every
account: pitch shifter + modulated reverb and/or modulated delay + feedback/EQ via a mixer.

Two details worth carrying forward:

- **Lexicon 224 "Concert Hall" character** (per the
  [Eno/Lanois post](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/),
  citing [Kevin Killen on Gearslutz](http://www.gearslutz.com/board/1112956-post2.html)):
  low initial echo density that *builds* to higher density as the tail decays, with heavy
  modulation — not an accurate room simulation, but "lush and spatially expansive." This is
  the same "echo density grows over time" property Gerzon's FDN work (Part 2, papers #3–#4,
  [source](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/))
  achieves structurally — the Lexicon 224 is a real-world, commercially iconic example of
  why that property matters perceptually, not just theoretically.
- **Harmonizer "de-glitch" artifacts** (per the
  [Eno/Lanois post](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/),
  citing [David Kulka on Gearslutz](http://www.gearslutz.com/board/2053584-post3.html)):
  early pitch shifters (AMS, Eventide H910) spliced waveform segments to change pitch; when
  the spliced-in/out points had different voltage levels, a small DC pop/crackle resulted,
  worse at extreme pitch settings. Both AMS and Eventide (on the H949) added "de-glitch"
  cards — smarter splice-point matching — to fix this. **This is the exact problem our
  dual-delay-line crossfade pitch shifter's crossfade is solving**, and it's also directly
  related to the "DC offset" pitfall already listed in `shimmer-reverb-concepts.md`. Worth
  remembering as historical grounding for *why* the crossfade quality matters, not just
  that it's needed.

## Actionable ideas not yet in our docs

Cross-checked against `shimmer-reverb-concepts.md`'s existing pitfalls/roadmap/references —
these are genuinely new angles, not restatements:

1. **Per-line damping tuned for equal decay rate** (Jot & Chaigne, paper #8 above; per
   [Valhalla Part 2](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/)) —
   our "Metallic ringing" pitfall in `shimmer-reverb-concepts.md` is currently mitigated
   only by choosing mutually-prime-ish delay lengths. Jot & Chaigne's technique is the
   *other* half of that fix: design each delay line's damping filter so every line loses
   energy at the same rate at every frequency, which is specifically what prevents some
   eigenmodes from ringing longer than others. Worth an explicit look when tuning
   `DattorroTank`'s damping filters, not just its delay lengths.
2. **Gerzon's rotation-matrix stereo spread** (paper #3 above; per
   [Valhalla Part 2](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/)) —
   a more principled alternative/addition to the "Mono collapse" pitfall's current fix list
   (different delay lengths/modulation per channel, or quadrature-offset pitch shift).
   Gerzon's rotation angle directly parameterizes how much a unitary-matrix FDN spreads
   energy between L/R — worth comparing against whatever decorrelation method Phase 4 of
   the implementation plan lands on.
3. **Slow delay-length modulation on the tank itself** (Stautner/Puckette, paper #6 above,
   [PDF](https://www.ee.columbia.edu/~dpwe/e4896/papers/StautP82-reverb.pdf); EMT-250
   precedent noted in the same source, [Valhalla Part 2](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/)) —
   our docs currently only discuss modulation in the context of the *shimmer pitch
   shifter*. This is a separate, older technique: modulating the **tank's own** delay
   lengths with a slow random/LFO signal measurably improves perceived reverb quality
   independent of shimmer, and predates shimmer effects entirely (commercial units since
   1976). Worth evaluating on `DattorroTank` even in a non-shimmer signal path.
4. **Gardner's nested allpass delays** (paper #9 above; per
   [Valhalla Part 2](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/)) —
   an allpass embedded inside another allpass's feedback/feedforward path, as a way to add
   density without a full extra tank stage. Not currently discussed anywhere in our docs;
   worth knowing as an option if the input diffuser ever needs more density without
   changing its allpass count.
5. **Spin Semiconductor's informal notes** (flagged in
   [Valhalla Part 3](https://valhalladsp.com/2021/09/23/getting-started-with-reverb-design-part-3-online-resources/);
   notes themselves at [spinsemi.com/knowledge_base.html](http://www.spinsemi.com/knowledge_base.html)) —
   a plain-language, implementation-level allpass-loop walkthrough. Good sanity-check
   reading before/while tuning `DattorroTank`'s coefficients, independent of the FV-1 chip
   itself.
6. **Modulating the tank's own delays as historically-grounded practice, not just theory**
   (per the [Eno/Lanois post](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/)'s
   Lexicon 224 "Concert Hall" description) — reinforces idea 3 above from the commercial
   side: the Lexicon 224's heavy internal modulation is exactly this technique, already in
   use on a hit-record-defining unit years before Stautner/Puckette published it.

## Notable overlap with existing docs

`shimmer-reverb-concepts.md` already cites Dattorro (#10) and Julius O. Smith's PASP (#7's
author) directly, and Moorer (#5) by name. This research pass adds full citation detail plus
Costello's own annotations for those, and fills in five papers our docs didn't cite at all
(Schroeder ×2, Gerzon ×2, Stautner/Puckette) that are the direct lineage leading to Dattorro's
1997 paper — useful if we ever need to explain *why* the Dattorro topology looks the way it
does, not just *that* we chose it.

## References

- Schroeder, "'Colorless' Artificial Reverberation," JAES, 1961
- Schroeder, "Natural Sounding Artificial Reverberation," JAES, July 1962
- Gerzon, "Synthetic Stereo Reverberation (Part 1)," Studio Sound, Dec 1971, pp.632–635
- Gerzon, "Synthetic Studio Reverberation (Part 2)," Studio Sound, Jan 1972, pp.24–28
- Moorer, "About This Reverberation Business," Computer Music Journal, June 1979
- Stautner & Puckette, "Designing Multi-Channel Reverberators," Computer Music Journal, Spring 1982 — [PDF](https://www.ee.columbia.edu/~dpwe/e4896/papers/StautP82-reverb.pdf)
- Smith, "A New Approach to Digital Reverberation using Closed Waveguide Networks," ICMC 1985, pp.47–53
- Jot & Chaigne, "Digital delay networks for designing artificial reverberators," AES 90th Convention, Feb 1991
- Gardner, "A Realtime Multichannel Room Simulator," 124th ASA meeting, Nov 1992
- Dattorro, "Effect Design, Part 1: Reverberator and Other Filters," JAES 45(9), Sept 1997 — [full PDF](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf) (already in `shimmer-reverb-concepts.md`)
- Valhalla DSP, ["Getting Started With Reverb Design, Part 2: The Best Papers"](https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/)
- Valhalla DSP, ["Getting Started With Reverb Design, Part 3: Online Resources"](https://valhalladsp.com/2021/09/23/getting-started-with-reverb-design-part-3-online-resources/)
- Valhalla DSP, ["Eno/Lanois Shimmer Sound: How it is made"](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/)
- Kevin Killen, U2 "4th of July" signal path — [Gearslutz thread](http://www.gearslutz.com/board/1112956-post2.html) (quoted in the Eno/Lanois post above)
- David Kulka on harmonizer de-glitch circuitry — [Gearslutz post](http://www.gearslutz.com/board/2053584-post3.html) (quoted in the Eno/Lanois post above)
- Spin Semiconductor — [www.spinsemi.com](http://www.spinsemi.com), [informal notes](http://www.spinsemi.com/knowledge_base.html), [example algorithms](http://www.spinsemi.com/programs.php)
- Puckette, *Theory and Techniques of Electronic Music* — [msp.ucsd.edu/techniques.htm](http://msp.ucsd.edu/techniques.htm)
- Smith, *Physical Audio Signal Processing* — [ccrma.stanford.edu/~jos/pasp/](https://ccrma.stanford.edu/~jos/pasp/)
- DAFx conference — [dafx.de](https://www.dafx.de), [paper archive](https://www.dafx.de/paper-archive/search.php?years=1998)
- AES E-Library — [aes.org/e-lib](https://www.aes.org/e-lib/)

## Related

- [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md) — conceptual guide these notes supplement
- [`shimmer-reverb-open-source-survey.md`](./shimmer-reverb-open-source-survey.md) — the equivalent survey for open-source *implementations* rather than academic papers
- [`shimmer-reverb-implementation-plan.md`](./shimmer-reverb-implementation-plan.md) — phased build plan these notes may inform but don't change

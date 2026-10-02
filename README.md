# MemoMind — Respiration / cohérence cardiaque (breathing)

Application **appairée** MemoMind : guide de respiration avec rythme visuel affiché sur les lunettes, configuré et suivi depuis le téléphone.

## Composition

| Côté | Dossier | Artefact |
| --- | --- | --- |
| Lunettes (plugin natif C) | `glass/breathing/` | `builds/breathing.gmp` |
| Téléphone (plugin Web) | `phone/breathing/` | `builds/breathing-1.0.0.mmpkg` |

## Fonctionnement

- Les lunettes affichent une barre animée qui grandit à l'inspiration et rétrécit à l'expiration, avec la phase courante (« Inspire », « Expire », « Retiens »), le décompte de la phase, le nombre de cycles et le temps restant.
- Le téléphone choisit le rythme parmi trois préréglages :
  - **Cohérence** 5 s / 5 s (6 respirations/min)
  - **Relaxation** 4 s / 6 s
  - **Carré** 4 – 4 – 4 – 4
- Durée de séance réglable de 1 à 30 min. Démarrage / pause / arrêt depuis le téléphone ; appui court sur le bouton des lunettes pour démarrer ou mettre en pause, appui long pour réinitialiser.
- Le téléphone journalise les séances terminées : séances du jour, des 7 derniers jours et minutes cumulées.

## Protocole binaire

`SETTINGS_CHANNEL = 0x5245` ('RE', téléphone → lunettes), `EVENT_CHANNEL = 0x5246` ('RF', lunettes → téléphone), version 1.

- `COMMAND_CONFIG` = `[version, cmd, preset, inhale, hold_in, exhale, hold_out, duration_min]` (8 octets)
- `COMMAND_START` / `COMMAND_PAUSE` / `COMMAND_STOP` (paddés à 8 octets)
- `EVENT_SESSION_DONE` = `[version, event, cycles u16, seconds u16, preset]`

## Build

Nécessite le SDK officiel MemoMind (`memomind-open/plugin-open-platform`). Déposer `glass/breathing/` dans `GlassSDK/examples/` et `phone/breathing/` dans `PhoneSDK/examples/`, puis lancer `python3 build.py`.

- Lunettes : `python3 build.py glass --force` → `.gmp`
- Téléphone : `python3 build.py web --force` → `.mmpkg`

Bilingue FR/EN côté lunettes.

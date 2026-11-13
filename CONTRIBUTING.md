# Contributing to PersonalDepth_pi / Contribuer à PersonalDepth_pi

[English](#english) · [Français](#français)

---

## English

Thank you for your interest! Contributions of any size are welcome: a test
report, a translation fix, a bug fix or a new feature. English or French, as
you prefer.

### Ways to contribute

- **Report a problem or a test result**: open an
  [issue](https://github.com/margouilla/PersonalDepth_pi/issues) with the
  *Problem report* or *Test report* form.
- **Discuss**: the
  [Cruisers Forum thread](https://www.cruisersforum.com/forums/f134/plugin-dev-dynamic-bathymetry-and-topography-grid-overlay-302721.html).
- **Translations**: see below — no programming or build needed.
- **Code**: through a pull request (see below).

### Translations

The interface translations are text files in `data/lang/`, one per language:
`PersonalDepth_<code>.po` (gettext format), for example
`PersonalDepth_ru.po`.

- `msgid` is the English text, `msgstr` its translation; the original French
  text is given as a comment (`#. fr:`). An empty `msgstr` shows English.
- Keep the `%d`, `%s`, `%.2f`… of the English text, in the same order (an
  entry whose placeholders differ is ignored by the plugin). `\n` is a line
  break.
- To test without rebuilding: put your copy in the `lang` folder of the
  plugin's user folder (see the user guide, *Translation files*) and restart
  OpenCPN.

To propose your translation, the simplest way is on GitHub itself: open the
file (e.g. `data/lang/PersonalDepth_ru.po`), click the pencil (*Edit this
file*), make your changes, then *Propose changes*: GitHub creates your copy of
the repository and the pull request for you. You can also attach the file to
an issue.

A new language: ask in an issue and a template will be generated
(`tools/lang/make_po.cpp`), or copy `PersonalDepth_ru.po`, empty the `msgstr`
lines and rename it with your language code (`de`, `es`, `it`…).

### Code: pull requests

1. **Fork** the repository, then create a branch from `main`
   (e.g. `fix-import-dates`).
2. **Build and test** locally:
   - Windows (OpenCPN 32 bits): `powershell -ExecutionPolicy Bypass -File build.ps1`
     (Visual Studio, wxWidgets 3.2.6; see the README);
   - Linux / macOS: `cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)`.
3. **Open a pull request** to `main`. GitHub Actions then build the plugin and
   run the tests on Windows, Linux x86_64, Raspberry Pi (arm64, armhf) and
   macOS; the result is shown on the pull request. For a first contribution,
   the maintainer has to approve the run (GitHub security rule).
4. The pull request is merged once the checks are green and the change is
   reviewed.

#### Guidelines

- C++17, wxWidgets 3.2, OpenCPN plugin API 1.18 (`api-21`). Keep the plugin
  working on every platform above, including 32-bit Windows (2 GB of address
  space: large data is read by blocks).
- `src/core/` has no wxWidgets dependency and is covered by
  `tests/test_core.cpp`: add a test with each fix or feature of the core.
- Texts shown to the user are written in French in the code (`U8("…")`,
  `pd::tr("…")`); add the English entry to `src/core/LangTable.inc` and the
  Russian one to `src/core/LangTableRu.inc` (or leave it for a translator),
  then regenerate `data/lang/PersonalDepth_ru.po`:

  ```
  g++ -std=c++17 -o make_po tools/lang/make_po.cpp src/core/Lang.cpp src/core/FileUtil.cpp
  ./make_po ru data/lang/PersonalDepth_ru.po
  ```
- Behaviour visible to users: update `CHANGELOG.md` (English and French) and
  the user guides (`docs/user-guide.md`, `docs/guide-utilisateur.md`).
- Never commit data under licence (e.g. Shom ENC charts or surveys) nor
  personal data, even as test files.

### Licence

By contributing, you agree that your contribution is distributed under the
project licence: GNU GPL version 2 or (at your option) any later version.

---

## Français

Merci de votre intérêt ! Toute contribution est la bienvenue, quelle que soit
sa taille : un retour de test, une correction de traduction, une correction de
bogue ou une nouvelle fonction. En français ou en anglais, comme vous
préférez.

### Comment contribuer

- **Signaler un problème ou un résultat de test** : ouvrir une
  [issue](https://github.com/margouilla/PersonalDepth_pi/issues) avec le
  formulaire *Signaler un problème* ou *Retour de test*.
- **Discuter** : le
  [fil Cruisers Forum](https://www.cruisersforum.com/forums/f134/plugin-dev-dynamic-bathymetry-and-topography-grid-overlay-302721.html).
- **Traductions** : voir ci-dessous — ni programmation ni compilation.
- **Code** : par une pull request (voir ci-dessous).

### Traductions

Les traductions de l'interface sont des fichiers texte dans `data/lang/`, un
par langue : `PersonalDepth_<code>.po` (format gettext), par exemple
`PersonalDepth_ru.po`.

- `msgid` est le texte anglais, `msgstr` sa traduction ; le texte français
  d'origine est en commentaire (`#. fr:`). Un `msgstr` vide affiche l'anglais.
- Garder les `%d`, `%s`, `%.2f`… du texte anglais, dans le même ordre (une
  entrée dont les spécificateurs diffèrent est ignorée par le plugin). `\n`
  est un saut de ligne.
- Pour essayer sans recompiler : placer sa copie dans le dossier `lang` du
  dossier utilisateur du plugin (voir le guide, *Fichiers de traduction*) et
  relancer OpenCPN.

Pour proposer sa traduction, le plus simple est de passer par GitHub : ouvrir
le fichier (ex. `data/lang/PersonalDepth_ru.po`), cliquer sur le crayon
(*Edit this file*), faire ses modifications, puis *Propose changes* : GitHub
crée la copie du dépôt et la pull request. On peut aussi joindre le fichier à
une issue.

Nouvelle langue : le demander dans une issue pour obtenir un modèle
(`tools/lang/make_po.cpp`), ou copier `PersonalDepth_ru.po`, vider les lignes
`msgstr` et le renommer avec le code de la langue (`de`, `es`, `it`…).

### Code : pull requests

1. **Forker** le dépôt, puis créer une branche depuis `main`
   (ex. `correction-dates-import`).
2. **Compiler et tester** localement :
   - Windows (OpenCPN 32 bits) : `powershell -ExecutionPolicy Bypass -File build.ps1`
     (Visual Studio, wxWidgets 3.2.6 ; voir le README) ;
   - Linux / macOS : `cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)`.
3. **Ouvrir une pull request** vers `main`. Les Actions GitHub compilent
   alors le plugin et lancent les tests sous Windows, Linux x86_64,
   Raspberry Pi (arm64, armhf) et macOS ; le résultat s'affiche sur la pull
   request. Pour une première contribution, le mainteneur doit autoriser le
   lancement (règle de sécurité de GitHub).
4. La pull request est fusionnée une fois les vérifications au vert et la
   modification relue.

#### Règles

- C++17, wxWidgets 3.2, API des plugins OpenCPN 1.18 (`api-21`). Le plugin
  doit fonctionner sur toutes les plateformes ci-dessus, y compris Windows
  32 bits (2 Go d'espace d'adressage : les gros fichiers sont lus par blocs).
- `src/core/` ne dépend pas de wxWidgets et est couvert par
  `tests/test_core.cpp` : ajouter un test avec chaque correction ou fonction
  du cœur.
- Les textes affichés sont écrits en français dans le code (`U8("…")`,
  `pd::tr("…")`) ; ajouter l'entrée anglaise dans `src/core/LangTable.inc`
  et la russe dans `src/core/LangTableRu.inc` (ou la laisser à un
  traducteur), puis régénérer `data/lang/PersonalDepth_ru.po` (commandes
  ci-dessus).
- Comportement visible des utilisateurs : mettre à jour `CHANGELOG.md`
  (anglais et français) et les guides (`docs/user-guide.md`,
  `docs/guide-utilisateur.md`).
- Ne jamais déposer de données sous licence (ex. cartes ou levés Shom) ni de
  données personnelles, même comme fichiers de test.

### Licence

En contribuant, vous acceptez que votre contribution soit diffusée sous la
licence du projet : GNU GPL version 2 ou (à votre choix) toute version
ultérieure.

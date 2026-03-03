# Katux — Un mini-système pour bidouilleurs sérieux (et drôles)

Bienvenue dans Katux : un petit projet d'OS/GUI léger pensé pour microcontrôleurs et écrans embarqués. Si vous aimez assembler des choses, regarder des pixels se déplacer et expliquer à vos amis "c'est pour un projet", vous êtes au bon endroit.

Ce README est rédigé à la main par une personne réelle qui boit trop de café et pas d'IA.

**Fonctionnalités principales**
- Interface graphique basique (composeur, fenêtres, curseur)
- Gestion d'applications simples (démos, réglages, navigateur local)
- Gestion des entrées (boutons, émulation souris)
- Kernel et scheduler minimal pour garder tout cela en ordre
- Écrans et pilotes adaptés pour plateformes hobby (voir dossiers `boards/` et `lib/`)

**Pourquoi Katux ?**

Parce que parfois on veut plus qu'un clignotement LED et moins qu'un système d'exploitation de la NASA. Katux vise à fournir une base simple et lisible pour expérimenter une interface sur microcontrôleur.

**Arborescence essentielle**
- `Katux.ino` : point d'entrée Arduino classique.
- `src/` : code source principal.
  - `core/` : noyau, scheduler, gestion d'événements.
  - `graphics/` : compositor, renderer, gestion des fenêtres et thème.
  - `apps/` : applications exemples (demo, settings...).
  - `input/` : gestion des boutons et émulation souris.
  - `system/` : BIOS, boot, gestion d'énergie, clavier logiciel.

**Prérequis et compilation**

Ouvrez le projet avec l'IDE Arduino ou utilisez PlatformIO si vous préférez rester dans une bulle VS Code :

1. Brancher la carte compatible (ex : cartes basées sur ESP ou autre MCU supporté).
2. Ouvrir `Katux.ino` dans l'IDE Arduino et sélectionner la carte et le port correspondants.
3. Cliquer sur "Upload".

Si vous utilisez PlatformIO, créez un environnement approprié ou adaptez `platformio.ini` (ce projet fournit des exemples dans d'autres dossiers du workspace).

Remarque : les paramètres exacts (résolution d'écran, brochage, drivers) dépendent du matériel — regardez `boards/` et `lib/` pour les adaptateurs et exemples.

**Contribution**

Contribuer est le meilleur compliment que vous puissiez faire au code. Pour contribuer :
- Ouvrez une issue pour discuter d'abord des changements importants.
- Envoyez des pull requests claires, ciblées et testées sur votre matériel (si possible).

Quelques bonnes pratiques : documentez le brochage matériel, fournissez un petit guide de test et évitez les commits du type "fixed stuff".

**Licence**

Ce dépôt n'inclut pas nécessairement un fichier `LICENSE` spécifique à Katux. Si vous avez besoin de réutiliser le code publiquement, ajoutez ou vérifiez la licence appropriée au préalable.

**Avertissements**

- Ce projet est orienté maker/hobby. Attendez-vous à bricoler.
- Les performances et la stabilité dépendent fortement du matériel cible.

Si vous lisez ceci et que vous pensez "c'est cool", alors ma mission est accomplie. Si vous pensez "c'est bancal", parfait — ouvrez une issue ou un PR, on améliorera ça ensemble.

-- L'équipe humaine (oui, vraiment)

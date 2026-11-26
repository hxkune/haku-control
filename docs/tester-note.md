# Tester note: template

A ready-to-send note for testers, in Russian and in French. Copy the part in the tester's language, fill in what this
build asks to check under "This time", and keep the three standing checks: they go with every build.
The full device lists are in [hardware-check.md](hardware-check.md).

---

## RU

**Сборка:** haku X.Y.Z
**Что проверить в этот раз:**
1. …
2. …

**Постоянные проверки (каждая сборка):**
1. **Подсветка.** Включи эффект (например Flow) на всех своих устройствах на 5 минут.
   - Цвета на устройствах такие же, как на карточке, ничего не мигает и не прыгает в другой цвет.
   - Выключи свет в haku: всё гаснет сразу и остаётся тёмным. Включи: эффект возвращается.
2. **Сон и пробуждение.** Отправь ПК в сон, разбуди.
   - Подсветка и вентиляторы снова под haku в течение минуты, без перезапуска haku.
   - Пока ПК спал, вентиляторы были на своей собственной настройке, а не застыли на скорости haku.
3. **Возврат управления.** Закрой haku из трея.
   - Каждое устройство возвращается к своей настройке или к своей программе (iCUE, aquasuite, BIOS) за несколько секунд.
   - Запусти haku снова: он берёт устройства обратно.

**Что прислать:**
- модель устройства и прошивку, версию haku (Настройки → О программе и Pro) и сборку Windows;
- что делал по шагам, что ожидалось и что было на самом деле;
- лог: Настройки → Помощь и диагностика → лог (`haku-control.log`, после вылета ещё `haku-control.prev.log`).

Не присылай ключи, пароли, PIN-коды и лицензионные ключи.
Если пункт не получилось проверить (нет такого устройства), так и напиши: «не проверено». Это не «работает».

---

## FR

**Version :** haku X.Y.Z
**À vérifier cette fois :**
1. …
2. …

**Vérifications permanentes (à chaque version) :**
1. **Éclairage.** Lance un effet (par exemple Flow) sur tous tes appareils pendant 5 minutes.
   - Les couleurs sur les appareils sont les mêmes que sur la carte, rien ne clignote ni ne saute à une autre couleur.
   - Éteins la lumière dans haku : tout s'éteint d'un coup et reste éteint. Rallume : l'effet revient.
2. **Veille et réveil.** Mets le PC en veille, puis réveille-le.
   - L'éclairage et les ventilateurs reviennent sous haku en moins d'une minute, sans relancer haku.
   - Pendant la veille, les ventilateurs tournaient selon leur propre réglage, pas figés à la vitesse de haku.
3. **Rendre la main.** Ferme haku depuis la zone de notification.
   - Chaque appareil revient à son propre réglage ou à son logiciel (iCUE, aquasuite, BIOS) en quelques secondes.
   - Relance haku : il reprend les appareils.

**À envoyer :**
- le modèle de l'appareil et son firmware, la version de haku (Réglages → À propos et Pro) et la version de Windows ;
- ce que tu as fait étape par étape, ce qui était attendu et ce qui s'est réellement passé ;
- le journal : Réglages → Aide et diagnostic → le journal (`haku-control.log`, après un plantage aussi
  `haku-control.prev.log`).

N'envoie ni clés, ni mots de passe, ni codes PIN, ni clés de licence.
Si un point n'a pas pu être vérifié (pas cet appareil), écris-le : « non vérifié ». Ce n'est pas « ça marche ».

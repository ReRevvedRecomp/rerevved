#include "title_text.h"

#include <array>
#include <cctype>
#include <unordered_map>

namespace rerevved::title_text
{

namespace
{

// Columns: English, German, French, Spanish, Italian. Empty translations use English, and \u escapes keep the source ASCII.
constexpr std::array<Row, kIdCount> kIdRows{ {
    { "title.exit_game_row", { "Exit Game", "Spiel beenden", "Quitter le jeu", "Salir del juego", "Esci dal gioco" }, "Special Functions menu row, shown last. Game text path." },
    { "page.options", { "Options", "Optionen", "Options", "Opciones", "Opzioni" }, "Page strip title and banner of the Options page. One short word." },
    { "page.achievements", { "Achievements", "Erfolge", "Succ\u00e8s", "Logros", "Obiettivi" }, "Page strip title and banner of the Achievements page. One short word." },
    { "achievements.summary", { "{0} of {1} unlocked", "{0} von {1} freigeschaltet", "{0} sur {1} d\u00e9verrouill\u00e9s", "{0} de {1} desbloqueados", "{0} su {1} sbloccati" }, "Panel title strip of the Achievements page. {0} unlocked, {1} total." },
    { "achievements.score", { "{0}G of {1}G", "{0}G von {1}G", "{0}G sur {1}G", "{0}G de {1}G", "{0}G su {1}G" }, "Right end of the same strip. {0} gamerscore earned, {1} possible; G is the gamerscore symbol." },
    { "achievements.locked", { "Locked", "Gesperrt", "Verrouill\u00e9", "Bloqueado", "Bloccato" }, "State line of a locked achievement row. Right-hand column, about 13 em wide." },
    { "achievements.unlocked", { "Unlocked", "Freigeschaltet", "D\u00e9verrouill\u00e9", "Desbloqueado", "Sbloccato" }, "State line of an unlocked achievement with no known date." },
    { "achievements.unlocked_on", { "Unlocked {0}", "Freigeschaltet am {0}", "D\u00e9verrouill\u00e9 le {0}", "Desbloqueado el {0}", "Sbloccato il {0}" }, "State line of an unlocked achievement. {0} is the date as YYYY-MM-DD." },
    { "achievements.none", { "No achievements are available.", "Keine Erfolge verf\u00fcgbar.", "Aucun succ\u00e8s disponible.", "No hay logros disponibles.", "Nessun obiettivo disponibile." }, "Centered in the list when no achievement is registered." },
    { "page.mods", { "Mods", "Mods", "Mods", "Mods", "Mod" }, "Page strip title and banner of the Mods page. One short word." },
    { "mods.summary", { "{0} installed, {1} enabled", "{0} installiert, {1} aktiviert", "{0} install\u00e9s, {1} activ\u00e9s", "{0} instalados, {1} activados", "{0} installati, {1} attivi" }, "Panel title strip of the Mods page. {0} installed, {1} enabled." },
    { "mods.none", { "No mod packages are installed.", "Keine Mod-Pakete installiert.", "Aucun paquet de mod install\u00e9.", "No hay paquetes de mods instalados.", "Nessun pacchetto mod installato." }, "Centered in the Packages list when no mod package is installed." },
    { "mods.native", { "Native", "Nativ", "Natif", "Nativo", "Nativo" }, "Tag at the right of a native (DLL) mod row. About 8 em wide." },
    { "mods.unavailable", { "{0} (unavailable)", "{0} (nicht verf\u00fcgbar)", "{0} (indisponible)", "{0} (no disponible)", "{0} (non disponibile)" }, "Title of a row for a package that is enabled but not installed. {0} is the package ID." },
    { "mods.unavailable_tip", { "This package is enabled but not installed. Remove it from the loadout.", "Dieses Paket ist aktiviert, aber nicht installiert. Aus der Mod-Auswahl entfernen.", "Ce paquet est activ\u00e9, mais pas install\u00e9. Retirez-le de la configuration.", "Este paquete est\u00e1 activado, pero no instalado. Qu\u00edtalo de la configuraci\u00f3n.", "Questo pacchetto \u00e8 attivo ma non installato. Rimuovilo dalla configurazione." }, "Second line of that row. Warns in gold; one line." },
    { "mods.moving", { "Moving: Up and Down change the load order.", "Verschieben: Hoch und Runter \u00e4ndern die Ladereihenfolge.", "D\u00e9placement\u00a0: Haut et Bas changent l'ordre de chargement.", "Mover: Arriba y Abajo cambian el orden de carga.", "Spostamento: Su e Gi\u00f9 cambiano l'ordine di caricamento." }, "Footer note while a mod is picked up for reordering." },
    { "mods.unsaved", { "Unsaved changes", "Ungespeicherte \u00c4nderungen", "Modifications non enregistr\u00e9es", "Cambios sin guardar", "Modifiche non salvate" }, "Footer note while changes are pending." },
    { "mods.restart", { "Restart required for saved changes.", "Gespeicherte \u00c4nderungen erfordern einen Neustart.", "Red\u00e9marrage requis pour les modifications enregistr\u00e9es.", "Los cambios guardados requieren reiniciar.", "Le modifiche salvate richiedono un riavvio." }, "Footer note after a save that needs a restart." },
    { "mods.fix_errors", { "Fix errors before saving.", "Vor dem Speichern Fehler beheben.", "Corrigez les erreurs avant d'enregistrer.", "Corrige los errores antes de guardar.", "Correggi gli errori prima di salvare." }, "Footer note when the selected loadout has errors; a detail may follow." },
    { "mods.replace_file", { "Save changes to replace the invalid loadout.", "\u00c4nderungen speichern, um die ung\u00fcltige Mod-Auswahl zu ersetzen.", "Enregistrez les modifications pour remplacer la configuration invalide.", "Guarda los cambios para sustituir la configuraci\u00f3n no v\u00e1lida.", "Salva le modifiche per sostituire la configurazione non valida." }, "Footer note when the saved loadout file is invalid; a detail may follow." },
    { "mods.save_failed", { "Could not save the mod loadout.", "Die Mod-Auswahl konnte nicht gespeichert werden.", "Impossible d'enregistrer la configuration des mods.", "No se ha podido guardar la configuraci\u00f3n de mods.", "Impossibile salvare la configurazione delle mod." }, "Footer note when saving fails and the runtime gave no reason." },
    { "mods.discard_ask", { "Discard enabled and load-order changes?", "\u00c4nderungen an Aktivierung und Ladereihenfolge verwerfen?", "Ignorer les changements d'activation et d'ordre de chargement\u00a0?", "\u00bfDescartar los cambios de activaci\u00f3n y orden de carga?", "Scartare le modifiche all'attivazione e all'ordine di caricamento?" }, "Question in the footer when closing with pending changes. Up to 3 lines." },
    { "mods.replace_ask", { "The current profile loadout has errors. Replace it with the selected packages?", "Die Mod-Auswahl des aktuellen Profils enth\u00e4lt Fehler. Durch die gew\u00e4hlten Pakete ersetzen?", "La configuration du profil actuel contient des erreurs. La remplacer par les paquets s\u00e9lectionn\u00e9s\u00a0?", "La configuraci\u00f3n del perfil actual tiene errores. \u00bfSustituirla por los paquetes seleccionados?", "La configurazione del profilo attuale contiene errori. Sostituirla con i pacchetti selezionati?" }, "Question in the footer before replacing an invalid saved loadout. Up to 3 lines." },
    { "legend.enable", { "Enable", "Aktivieren", "Activer", "Activar", "Attiva" }, "Footer button caption (A) on a disabled mod. Button fits about 10 characters." },
    { "legend.disable", { "Disable", "Abschalten", "D\u00e9sactiver", "Desactivar", "Disattiva" }, "Footer button caption (A) on an enabled mod. Button fits about 10 characters." },
    { "legend.remove", { "Remove", "Entfernen", "Retirer", "Quitar", "Rimuovi" }, "Footer button caption (A) on an unavailable package. Button fits about 10 characters." },
    { "legend.move", { "Move", "Bewegen", "D\u00e9placer", "Mover", "Sposta" }, "Footer button caption (X) to pick up a mod. Button fits about 10 characters." },
    { "legend.drop", { "Drop", "Ablegen", "Poser", "Soltar", "Posa" }, "Footer button caption (A) while a mod is picked up. Button fits about 10 characters." },
    { "legend.apply", { "Apply", "Sichern", "Valider", "Aplicar", "Applica" }, "Footer button caption (Y) to save the loadout. Button fits about 10 characters." },
    { "legend.discard", { "Discard", "Verwerfen", "Ignorer", "Descartar", "Scarta" }, "Footer button caption (A) answering the discard question. Button fits about 10 characters." },
    { "legend.replace", { "Replace", "Ersetzen", "Remplacer", "Sustituir", "Sostituisci" }, "Footer button caption (A) answering the replace question. Button fits about 10 characters." },
    { "legend.keep", { "Keep", "Behalten", "Garder", "Mantener", "Tieni" }, "Footer button caption (B) answering either question. Button fits about 10 characters." },
    { "legend.cancel", { "Cancel", "Abbrechen", "Annuler", "Cancelar", "Annulla" }, "Footer button caption (B) while a mod is picked up. Button fits about 10 characters." },
    { "achievements.tab_original", { "Original", "Original", "Originaux", "Originales", "Originali" }, "Tab of the game's own achievements. One short word." },
    { "achievements.tab_custom", { "Custom", "Eigene", "Personnalis\u00e9s", "Personalizados", "Personalizzati" }, "Tab of the title's custom achievements. One short word." },
    { "mods.tab_packages", { "Packages", "Pakete", "Paquets", "Paquetes", "Pacchetti" }, "Mods page tab of the mod packages. One short word." },
    { "mods.tab_assets", { "Assets", "Spieldaten", "Ressources", "Archivos", "Risorse" }, "Mods page tab of the asset override packs. One short word." },
    { "assets.none", { "No asset override packs are installed.", "Keine Pakete zum Ersetzen von Spieldaten installiert.", "Aucun pack de remplacement de ressources install\u00e9.", "No hay paquetes de sustituci\u00f3n de archivos instalados.", "Nessun pacchetto di sostituzione delle risorse installato." }, "Centered in the Assets list when no asset override pack is installed." },
    { "assets.moving", { "Moving: Up and Down change the priority order.", "Verschieben: Hoch und Runter \u00e4ndern die Priorit\u00e4tsreihenfolge.", "D\u00e9placement\u00a0: Haut et Bas changent l'ordre de priorit\u00e9.", "Mover: Arriba y Abajo cambian el orden de prioridad.", "Spostamento: Su e Gi\u00f9 cambiano l'ordine di priorit\u00e0." }, "Footer note while an asset pack is picked up for reordering." },
    { "assets.save_failed", { "Could not save the asset overlay loadout.", "Die Auswahl der Spieldatenpakete konnte nicht gespeichert werden.", "Impossible d'enregistrer la configuration des ressources de remplacement.", "No se ha podido guardar la configuraci\u00f3n de sustituci\u00f3n de archivos.", "Impossibile salvare la configurazione delle risorse sostitutive." }, "Footer note when saving asset packs fails and the runtime gave no reason." },
    { "assets.discard_ask", { "Discard asset overlay enable and priority changes?", "\u00c4nderungen an Aktivierung und Priorit\u00e4t der Spieldatenpakete verwerfen?", "Ignorer les changements d'activation et de priorit\u00e9 des ressources de remplacement\u00a0?", "\u00bfDescartar los cambios de activaci\u00f3n y prioridad de los archivos de sustituci\u00f3n?", "Scartare le modifiche all'attivazione e alla priorit\u00e0 delle risorse sostitutive?" }, "Question in the footer when closing with pending asset changes. Up to 3 lines." },
    { "assets.replace_ask", { "The current asset overlay loadout has errors. Replace it with the selected packs?", "Die aktuelle Auswahl der Spieldatenpakete enth\u00e4lt Fehler. Durch die gew\u00e4hlten Pakete ersetzen?", "La configuration des ressources de remplacement contient des erreurs. La remplacer par les packs s\u00e9lectionn\u00e9s\u00a0?", "La configuraci\u00f3n de sustituci\u00f3n de archivos tiene errores. \u00bfSustituirla por los paquetes seleccionados?", "La configurazione delle risorse sostitutive contiene errori. Sostituirla con i pacchetti selezionati?" }, "Question in the footer before replacing an invalid asset loadout. Up to 3 lines." },
    { "mods.tab_maps", { "Maps", "Karten", "Cartes", "Mapas", "Mappe" }, "Mods page tab of the map packs. One short word." },
    { "maps.none", { "No map packs are installed.", "Keine Kartenpakete installiert.", "Aucun pack de cartes install\u00e9.", "No hay paquetes de mapas instalados.", "Nessun pacchetto di mappe installato." }, "Centered in the Maps list when no map pack is installed." },
    { "maps.moving", { "Moving: Up and Down change the order.", "Verschieben: Hoch und Runter \u00e4ndern die Reihenfolge.", "D\u00e9placement\u00a0: Haut et Bas changent l'ordre.", "Mover: Arriba y Abajo cambian el orden.", "Spostamento: Su e Gi\u00f9 cambiano l'ordine." }, "Footer note while a map pack is picked up for reordering." },
    { "maps.save_failed", { "Could not save the map pack loadout.", "Die Auswahl der Kartenpakete konnte nicht gespeichert werden.", "Impossible d'enregistrer la configuration des packs de cartes.", "No se ha podido guardar la configuraci\u00f3n de los paquetes de mapas.", "Impossibile salvare la configurazione dei pacchetti di mappe." }, "Footer note when saving map packs fails and no reason was given." },
    { "maps.discard_ask", { "Discard map pack enable and order changes?", "\u00c4nderungen an Aktivierung und Reihenfolge der Kartenpakete verwerfen?", "Ignorer les changements d'activation et d'ordre des packs de cartes\u00a0?", "\u00bfDescartar los cambios de activaci\u00f3n y orden de los paquetes de mapas?", "Scartare le modifiche all'attivazione e all'ordine dei pacchetti di mappe?" }, "Question in the footer when closing with pending map pack changes. Up to 3 lines." },
    { "maps.replace_ask", { "The current map pack loadout has errors. Replace it with the selected packs?", "Die aktuelle Auswahl der Kartenpakete enth\u00e4lt Fehler. Durch die gew\u00e4hlten Pakete ersetzen?", "La configuration des packs de cartes contient des erreurs. La remplacer par les packs s\u00e9lectionn\u00e9s\u00a0?", "La configuraci\u00f3n de los paquetes de mapas tiene errores. \u00bfSustituirla por los paquetes seleccionados?", "La configurazione dei pacchetti di mappe contiene errori. Sostituirla con i pacchetti selezionati?" }, "Question in the footer before replacing an invalid map pack loadout. Up to 3 lines." },
    { "legend.back", { "Back", "Zur\u00fcck", "Retour", "Atr\u00e1s", "Indietro" }, "Footer button caption (B), right end. Button fits about 10 characters. Retail menus say Back too." },
    { "legend.select", { "Select", "W\u00e4hlen", "Choisir", "Elegir", "Scegli" }, "Footer button caption (A) on the Options page. Button fits about 10 characters." },
    { "diagnostic.error", { "error: {0}", "Fehler: {0}", "erreur\u00a0: {0}", "error: {0}", "errore: {0}" }, "Prefix on an error message about a mod package. {0} is the message, shown as given." },
    { "diagnostic.warning", { "warning: {0}", "Warnung: {0}", "avertissement\u00a0: {0}", "advertencia: {0}", "avviso: {0}" }, "Prefix on a warning message about a mod package. {0} is the message, shown as given." },
    { "diagnostic.line", { "line {0}: {1}", "Zeile {0}: {1}", "ligne {0}\u00a0: {1}", "l\u00ednea {0}: {1}", "riga {0}: {1}" }, "A loadout problem with a line number. {0} is the line, {1} the message, shown as given." },
    { "achievements.gamerscore", { "{0}G", "{0}G", "{0}G", "{0}G", "{0}G" }, "Gamerscore of one achievement, top of the right-hand column. G is the gamerscore symbol." },
    { "mods.locked", { "Mods can't be changed during a multiplayer game.", "Mods k\u00f6nnen w\u00e4hrend eines Mehrspielerspiels nicht ge\u00e4ndert werden.", "Les mods ne peuvent pas \u00eatre modifi\u00e9s pendant une partie multijoueur.", "No se pueden cambiar los mods durante una partida multijugador.", "Non puoi modificare le mod durante una partita multigiocatore." }, "Footer note on the Mods page during a multiplayer game. Shown in place of the runtime's English reason." },
    { "key.shift", { "Shift", "Umschalt", "Maj", "May\u00fas", "Maiusc" }, "Caption of the text entry's Shift key and its footer button (Y). Fits about 9 characters." },
    { "key.space", { "Space", "Leertaste", "Espace", "Espacio", "Spazio" }, "Caption of the text entry's Space key. Fits about 9 characters." },
    { "key.paste", { "Paste", "Einf\u00fcgen", "Coller", "Pegar", "Incolla" }, "Caption of the text entry's Paste key. Pastes the clipboard. Fits about 9 characters." },
    { "key.clear", { "Clear", "Leeren", "Vider", "Vaciar", "Svuota" }, "Caption of the text entry's Clear key. Removes all the text. Fits about 9 characters." },
    { "key.delete", { "Delete", "L\u00f6schen", "Effacer", "Borrar", "Elimina" }, "Caption of the text entry's Delete key and its footer button (X). Removes the last character. Fits about 9 characters." },
    { "key.done", { "OK", "OK", "OK", "Aceptar", "OK" }, "Caption of the text entry's OK key and its footer button (Start or Enter). Applies the text. Fits about 9 characters." },
    { "legend.type", { "Type", "Eingeben", "Saisir", "Escribir", "Scrivi" }, "Footer button caption (A) on the text entry with a gamepad. Types the highlighted key. Fits about 9 characters." },
    { "text.hint", { "Type or paste a new value.", "Neuen Wert eingeben oder einf\u00fcgen.", "Saisissez ou collez une nouvelle valeur.", "Escribe o pega un valor nuevo.", "Scrivi o incolla un nuovo valore." }, "Dim line in the text entry while its field is empty. One line." },
    { "button.copy", { "Copy", "Kopieren", "Copier", "Copiar", "Copia" }, "Caption of the Invite Code row's Copy button and its footer button (Y). Copies the saved code to the clipboard. Paste uses key.paste. Fits about 9 characters." },
    { "legend.reset", { "Reset", "Zur\u00fccksetzen", "R\u00e9initialiser", "Restablecer", "Ripristina" }, "Keybind row's Reset button and its footer button (Y). Puts the default key back. Fits about 9 characters." },
    { "bind.press", { "Press a key...", "Taste dr\u00fccken ...", "Appuyez sur une touche...", "Pulsa una tecla...", "Premi un tasto..." }, "Value of a keybind row while it waits for the new key, centered in the value column (about 26 em wide)." },
} };

constexpr auto kKeyedRows = std::to_array<Row>({
    { "tab.general", { "General", "Allgemein", "G\u00e9n\u00e9ral", "General", "Generali" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "tab.gameplay", { "Gameplay", "Spiel", "Jeu", "Juego", "Gioco" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "tab.notifications", { "Notifications", "Meldungen", "Notifications", "Avisos", "Notifiche" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "tab.online", { "Online", "Online", "En ligne", "En l\u00ednea", "Online" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "tab.controls", { "Controls", "Steuerung", "Commandes", "Controles", "Comandi" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "tab.display", { "Display", "Anzeige", "Affichage", "Pantalla", "Schermo" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "tab.audio", { "Audio", "Audio", "Audio", "Sonido", "Audio" }, "Tab title in the strip under the page strip. One short word; about 14 characters at most." },
    { "section.ai_handicaps", { "AI Handicaps", "KI-Boni", "Bonus de l'IA", "Bonificaciones de la IA", "Bonus IA" }, "Subheading above a group of rows inside a tab." },
    { "section.game_of_the_week", { "Game of the Week", "Spiel der Woche", "Partie de la semaine", "Partida de la semana", "Gioco della settimana" }, "Subheading above a group of rows inside a tab." },
    { "section.connection", { "Connection", "Verbindung", "Connexion", "Conexi\u00f3n", "Connessione" }, "Subheading above a group of rows inside a tab." },
    { "section.matchmaking", { "Matchmaking", "Mitspieler-Suche", "Recherche de partie", "Buscar partida", "Ricerca partita" }, "Subheading above a group of rows inside a tab." },
    { "section.discord", { "Discord", "Discord", "Discord", "Discord", "Discord" }, "Subheading above a group of rows inside a tab." },
    { "setting.game_language.label", { "Language", "Sprache", "Langue", "Idioma", "Lingua" }, "" },
    { "setting.game_language.tip", { "Choose the game's language.", "Legt die Sprache des Spiels fest.", "Choisit la langue du jeu.", "Elige el idioma del juego.", "Scegli la lingua del gioco." }, "" },
    { "setting.show_fps_counter.label", { "FPS Counter", "FPS-Anzeige", "Compteur d'images/s", "Contador de FPS", "Contatore FPS" }, "" },
    { "setting.show_fps_counter.tip", { "Enable to show frames per second in the corner.", "Zeigt bei Aktivierung die Bilder pro Sekunde in einer Bildschirmecke an.", "Affiche le nombre d'images par seconde dans un coin de l'\u00e9cran.", "Muestra los fotogramas por segundo en una esquina de la pantalla.", "Mostra i fotogrammi al secondo in un angolo dello schermo." }, "" },
    { "setting.skip_intro_movie.label", { "Skip Intro", "Intro \u00fcberspringen", "Passer l'introduction", "Omitir introducci\u00f3n", "Salta l'introduzione" }, "" },
    { "setting.skip_intro_movie.tip", { "Enable to skip the legal screens and the opening movie at startup.", "\u00dcberspringt bei Aktivierung die rechtlichen Hinweise und das Intro beim Spielstart.", "Passe les mentions l\u00e9gales et la vid\u00e9o d'introduction au d\u00e9marrage.", "Omite los avisos legales y el v\u00eddeo de introducci\u00f3n al iniciar el juego.", "Salta gli avvisi legali e il filmato introduttivo all'avvio." }, "" },
    { "setting.open_log_folder.label", { "Open Log Folder", "Protokollordner \u00f6ffnen", "Ouvrir le dossier des journaux", "Abrir carpeta de registros", "Apri cartella dei log" }, "" },
    { "setting.open_log_folder.tip", { "Open the folder containing this session's log file.", "\u00d6ffnet den Ordner mit der Protokolldatei dieser Sitzung.", "Ouvre le dossier contenant le fichier journal de cette session.", "Abre la carpeta que contiene el archivo de registro de esta sesi\u00f3n.", "Apre la cartella con il file di log di questa sessione." }, "" },
    { "setting.combat_speed.label", { "Combat Speed", "Kampfgeschwindigkeit", "Vitesse des combats", "Velocidad de combate", "Velocit\u00e0 di combattimento" }, "" },
    { "setting.combat_speed.tip", { "Choose how quickly battle animations play.", "Legt die Geschwindigkeit der Kampfanimationen fest.", "R\u00e8gle la vitesse des animations de combat.", "Elige la velocidad de las animaciones de combate.", "Scegli la velocit\u00e0 delle animazioni di combattimento." }, "" },
    { "setting.resources.label", { "Resources", "Ressourcen", "Ressources", "Recursos", "Risorse" }, "" },
    { "setting.resources.tip", { "Choose how resources are placed around each starting location.", "Legt die Verteilung der Ressourcen um die Startpositionen fest.", "Choisit la r\u00e9partition des ressources autour de chaque position de d\u00e9part.", "Elige c\u00f3mo se distribuyen los recursos alrededor de cada posici\u00f3n inicial.", "Scegli come distribuire le risorse attorno a ogni posizione iniziale." }, "" },
    { "setting.raging_barbarians.label", { "Raging Barbarians", "W\u00fctende Barbaren", "Barbares d\u00e9cha\u00een\u00e9s", "B\u00e1rbaros furiosos", "Barbari furiosi" }, "" },
    { "setting.raging_barbarians.tip", { "Enable for more aggressive Barbarians. Barbarians cannot capture a civilization's capital before 3000 BC.", "Macht Barbaren aggressiver. Vor 3000 v. Chr. k\u00f6nnen sie keine Hauptstadt einer Zivilisation erobern.", "Rend les Barbares plus agressifs. Ils ne peuvent prendre la capitale d'une civilisation avant 3000 av. J.-C.", "Hace m\u00e1s agresivos a los B\u00e1rbaros. No pueden tomar la capital de una civilizaci\u00f3n antes del 3000 a. C.", "Rende i Barbari pi\u00f9 aggressivi. Non possono conquistare la capitale di una civilt\u00e0 prima del 3000 a.C." }, "" },
    { "setting.one_city_challenge.label", { "One City Challenge", "Ein-Stadt-Herausforderung", "D\u00e9fi \u00e0 une seule ville", "Desaf\u00edo de una sola ciudad", "Sfida con una sola citt\u00e0" }, "" },
    { "setting.one_city_challenge.tip", { "Enable to block Settler production.", "Verhindert bei Aktivierung die Ausbildung von Siedlern.", "Emp\u00eache la production de Colons.", "Impide la producci\u00f3n de Colonos.", "Impedisce la produzione di Coloni." }, "" },
    { "setting.domination_min_turns.label", { "Earliest Domination", "Fr\u00fchester Herrschaftssieg", "Premi\u00e8re victoire territoriale", "Dominaci\u00f3n m\u00e1s temprana", "Prima vittoria per dominazione" }, "" },
    { "setting.domination_min_turns.tip", { "Set the minimum turns before a Domination victory in new games. Loaded saves keep 10.", "Legt die Mindestrundenzahl f\u00fcr einen Herrschaftssieg in neuen Spielen fest. Geladene Spielst\u00e4nde behalten 10.", "Fixe le tour minimal de victoire territoriale des nouvelles parties. Les sauvegardes charg\u00e9es conservent 10.", "Fija el turno m\u00ednimo para una victoria por dominaci\u00f3n en nuevas partidas. Las partidas cargadas conservan 10.", "Imposta il turno minimo per la vittoria per dominazione nelle nuove partite. I salvataggi caricati mantengono 10." }, "" },
    { "setting.ai_civilizations.label", { "AI Civilizations", "KI-Zivilisationen", "Civilisations de l'IA", "Civilizaciones de la IA", "Civilt\u00e0 IA" }, "" },
    { "setting.ai_civilizations.tip", { "Set the number of AI opponents. Single Player uses 1 to 6. Multiplayer and Game of the Week use up to 4, and multiplayer can have none.", "Legt KI-Gegner fest. Einzelspieler: 1 bis 6. Mehrspieler und Spiel der Woche: bis zu 4. Mehrspieler auch ohne KI.", "R\u00e8gle le nombre d'IA. Solo\u00a0: 1 \u00e0 6. Multijoueur et Partie de la semaine\u00a0: jusqu'\u00e0 4. Aucune IA possible en multijoueur.", "Fija los rivales de la IA. Un jugador: 1 a 6. Multijugador y Partida de la semana: hasta 4. Multijugador permite 0.", "Imposta i rivali IA. Giocatore singolo: da 1 a 6. Multigiocatore e Gioco della settimana: fino a 4. In multigiocatore anche 0." }, "" },
    { "setting.ai_research_bonus_percent.label", { "AI Research Bonus", "KI-Forschungsbonus", "Bonus de recherche de l'IA", "Bonificaci\u00f3n de investigaci\u00f3n de IA", "Bonus ricerca IA" }, "" },
    { "setting.ai_research_bonus_percent.tip", { "Increase AI research speed by this percentage.", "Erh\u00f6ht die Forschungsgeschwindigkeit der KI um diesen Prozentsatz.", "Augmente la vitesse de recherche de l'IA de ce pourcentage.", "Aumenta la velocidad de investigaci\u00f3n de la IA en este porcentaje.", "Aumenta la velocit\u00e0 di ricerca dell'IA di questa percentuale." }, "" },
    { "setting.ai_production_bonus_percent.label", { "AI Production Bonus", "KI-Produktionsbonus", "Bonus de production de l'IA", "Bonificaci\u00f3n de producci\u00f3n de IA", "Bonus produzione IA" }, "" },
    { "setting.ai_production_bonus_percent.tip", { "Increase AI production speed by this percentage.", "Erh\u00f6ht die Produktionsgeschwindigkeit der KI um diesen Prozentsatz.", "Augmente la vitesse de production de l'IA de ce pourcentage.", "Aumenta la velocidad de producci\u00f3n de la IA en este porcentaje.", "Aumenta la velocit\u00e0 di produzione dell'IA di questa percentuale." }, "" },
    { "setting.ai_gold_bonus_percent.label", { "AI Gold Bonus", "KI-Goldbonus", "Bonus d'or de l'IA", "Bonificaci\u00f3n de oro de la IA", "Bonus oro IA" }, "" },
    { "setting.ai_gold_bonus_percent.tip", { "Increase gold from AI cities by this percentage.", "Erh\u00f6ht den Goldertrag der KI-St\u00e4dte um diesen Prozentsatz.", "Augmente l'or produit par les villes de l'IA de ce pourcentage.", "Aumenta el oro producido por las ciudades de la IA en este porcentaje.", "Aumenta l'oro prodotto dalle citt\u00e0 dell'IA di questa percentuale." }, "" },
    { "setting.ai_food_bonus_percent.label", { "AI Food Bonus", "KI-Nahrungsbonus", "Bonus de nourriture de l'IA", "Bonificaci\u00f3n de alimentos de la IA", "Bonus cibo IA" }, "" },
    { "setting.ai_food_bonus_percent.tip", { "Increase food growth in AI cities by this percentage. Starvation is unaffected.", "Erh\u00f6ht den Nahrungszuwachs in KI-St\u00e4dten um diesen Prozentsatz. Nahrungsmangel bleibt unver\u00e4ndert.", "Augmente les gains de nourriture des villes de l'IA de ce pourcentage. Les famines restent inchang\u00e9es.", "Aumenta la acumulaci\u00f3n de alimentos de las ciudades de la IA en este porcentaje. No afecta a las hambrunas.", "Aumenta l'accumulo di cibo nelle citt\u00e0 dell'IA di questa percentuale. Non influisce sulle carestie." }, "" },
    { "setting.ai_culture_bonus_percent.label", { "AI Culture Bonus", "KI-Kulturbonus", "Bonus de culture de l'IA", "Bonificaci\u00f3n de cultura de la IA", "Bonus cultura IA" }, "" },
    { "setting.ai_culture_bonus_percent.tip", { "Increase culture from AI cities by this percentage.", "Erh\u00f6ht den Kulturertrag der KI-St\u00e4dte um diesen Prozentsatz.", "Augmente la culture produite par les villes de l'IA de ce pourcentage.", "Aumenta la cultura producida por las ciudades de la IA en este porcentaje.", "Aumenta la cultura prodotta dalle citt\u00e0 dell'IA di questa percentuale." }, "" },
    { "setting.gotw_standard_rules.label", { "Standard Rules", "Standardregeln", "R\u00e8gles standard", "Reglas est\u00e1ndar", "Regole standard" }, "" },
    { "setting.gotw_standard_rules.tip", { "Enable to play Game of the Week without mods or gameplay changes other than Earliest Domination.", "Spielt das Spiel der Woche ohne Mods oder Regel\u00e4nderungen au\u00dfer Fr\u00fchester Herrschaftssieg.", "Joue la Partie de la semaine sans mods ni r\u00e8gles modifi\u00e9es, sauf Premi\u00e8re victoire territoriale.", "Juega la Partida de la semana sin mods ni cambios de reglas salvo Dominaci\u00f3n m\u00e1s temprana.", "Gioca al Gioco della settimana senza mod n\u00e9 regole modificate, tranne Prima vittoria per dominazione." }, "" },
    { "setting.gotw_custom_week.label", { "Custom Week", "Andere Woche w\u00e4hlen", "Choisir une autre semaine", "Elegir otra semana", "Scegli un'altra settimana" }, "" },
    { "setting.gotw_custom_week.tip", { "Enable to play a different week's game instead of this week's.", "Erm\u00f6glicht das Spiel einer anderen Woche statt der aktuellen.", "Permet de jouer la partie d'une autre semaine \u00e0 la place de celle de cette semaine.", "Permite jugar la partida de otra semana en lugar de la actual.", "Permette di giocare la partita di un'altra settimana al posto di quella attuale." }, "" },
    { "setting.gotw_week.label", { "Week", "Woche", "Semaine", "Semana", "Settimana" }, "" },
    { "setting.gotw_custom_civilization.label", { "Custom Civilization", "Zivilisation selbst w\u00e4hlen", "Choisir sa civilisation", "Elegir civilizaci\u00f3n", "Scegli la civilt\u00e0" }, "" },
    { "setting.gotw_custom_civilization.tip", { "Enable to play Game of the Week as the civilization you choose.", "Erm\u00f6glicht das Spiel der Woche mit einer selbst gew\u00e4hlten Zivilisation.", "Permet de jouer la Partie de la semaine avec la civilisation de votre choix.", "Permite jugar la Partida de la semana con la civilizaci\u00f3n que elijas.", "Permette di giocare al Gioco della settimana con la civilt\u00e0 che preferisci." }, "" },
    { "setting.gotw_civilization.label", { "Civilization", "Zivilisation", "Civilisation", "Civilizaci\u00f3n", "Civilt\u00e0" }, "" },
    { "setting.hide_research_prompt.label", { "Disable Research Advisor Prompt", "Forschungsberater ausblenden", "Masquer les demandes de recherche", "Omitir avisos del asesor cient\u00edfico", "Nascondi gli avvisi di ricerca" }, "" },
    { "setting.hide_research_prompt.tip", { "Enable to choose research yourself instead of receiving automatic prompts.", "Erm\u00f6glicht die eigene Forschungswahl ohne automatische Aufforderungen.", "Permet de choisir vos recherches sans recevoir de demandes automatiques.", "Permite elegir la investigaci\u00f3n sin recibir avisos autom\u00e1ticos.", "Permette di scegliere la ricerca senza ricevere richieste automatiche." }, "" },
    { "setting.hide_popup_city_status.label", { "Disable City Status Reports", "Stadtstatusberichte ausblenden", "Masquer les rapports des villes", "Omitir informes del estado de ciudades", "Nascondi i rapporti sulle citt\u00e0" }, "" },
    { "setting.hide_popup_city_status.tip", { "Enable to skip notices about disorder, celebrations, culture and blockades.", "Blendet Meldungen zu Unruhen, Feiern, Kultur und Blockaden aus.", "Masque les avis de troubles, de c\u00e9l\u00e9brations, de culture et de blocus.", "Omite los avisos sobre disturbios, celebraciones, cultura y bloqueos.", "Nasconde gli avvisi su disordini, festeggiamenti, cultura e blocchi." }, "" },
    { "setting.hide_popup_discoveries.label", { "Disable Discovery and Progress News", "Entdeckungs-/Fortschrittsmeldungen aus", "Masquer d\u00e9couvertes et progr\u00e8s", "Omitir descubrimientos y avances", "Nascondi scoperte e progressi" }, "" },
    { "setting.hide_popup_discoveries.tip", { "Enable to skip notices about technologies, eras, milestones, trade and roads.", "Blendet Meldungen zu Technologien, Zeitaltern, Meilensteinen, Handel und Stra\u00dfen aus.", "Masque les avis de technologies, d'\u00e8res, d'\u00e9tapes majeures, de commerce et de routes.", "Omite los avisos sobre tecnolog\u00edas, eras, hitos, comercio y carreteras.", "Nasconde gli avvisi su tecnologie, ere, traguardi, commercio e strade." }, "" },
    { "setting.hide_popup_combat_reports.label", { "Disable Combat Reports", "Kampfberichte ausblenden", "Masquer les rapports de combat", "Omitir informes de combate", "Nascondi i rapporti di combattimento" }, "" },
    { "setting.hide_popup_combat_reports.tip", { "Enable to skip notices about Great Generals, captures and promotions.", "Blendet Meldungen zu Gro\u00dfen Gener\u00e4len, Gefangennahmen und Bef\u00f6rderungen aus.", "Masque les avis de G\u00e9n\u00e9raux illustres, de captures et de promotions.", "Omite los avisos sobre Grandes generales, capturas y ascensos.", "Nasconde gli avvisi su Grandi Generali, catture e promozioni." }, "" },
    { "setting.hide_popup_rival_great_people.label", { "Disable Rival Great People and Wonders", "Rivalen: Pers\u00f6nlichkeiten/Wunder aus", "Masquer illustres et Merveilles rivaux", "Omitir personajes y maravillas rivales", "Nascondi personaggi e meraviglie rivali" }, "" },
    { "setting.hide_popup_rival_great_people.tip", { "Enable to skip notices about rival Great People and Wonders.", "Blendet Meldungen zu Gro\u00dfen Pers\u00f6nlichkeiten und Wundern anderer Zivilisationen aus.", "Masque les avis sur les Personnages illustres et les Merveilles des adversaires.", "Omite los avisos sobre Grandes personajes y Maravillas de los rivales.", "Nasconde gli avvisi sui Grandi Personaggi e sulle Meraviglie dei rivali." }, "" },
    { "setting.hide_popup_rankings.label", { "Disable Rankings and Culture News", "Ranglisten-/Kulturmeldungen ausblenden", "Masquer classements et culture", "Omitir clasificaciones y cultura", "Nascondi classifiche e cultura" }, "" },
    { "setting.hide_popup_rankings.tip", { "Enable to skip rankings, greatest city notices and culture flips.", "Blendet Ranglisten, Meldungen zur gr\u00f6\u00dften Stadt und kulturell bedingte Stadt\u00fcbertritte aus.", "Masque les classements, les avis de plus grande ville et les ralliements de villes par la culture.", "Omite clasificaciones, avisos de la mayor ciudad y cambios de bando de ciudades por influencia cultural.", "Nasconde classifiche, avvisi sulla citt\u00e0 pi\u00f9 grande e passaggi di citt\u00e0 dovuti all'influenza culturale." }, "" },
    { "setting.hide_popup_rival_war_peace.label", { "Disable Rival War and Peace News", "Krieg/Frieden der Rivalen ausblenden", "Masquer guerres et paix des rivaux", "Omitir guerras y paces de rivales", "Nascondi guerre e paci dei rivali" }, "" },
    { "setting.hide_popup_rival_war_peace.tip", { "Enable to skip notices when other civilizations declare war or make peace.", "Blendet Meldungen aus, wenn andere Zivilisationen Krieg erkl\u00e4ren oder Frieden schlie\u00dfen.", "Masque les avis de d\u00e9claration de guerre ou de paix des autres civilisations.", "Omite los avisos cuando otras civilizaciones declaran la guerra o firman la paz.", "Nasconde gli avvisi quando altre civilt\u00e0 dichiarano guerra o fanno pace." }, "" },
    { "setting.hide_popup_save_reminder.label", { "Disable Save Reminder", "Speichererinnerung ausblenden", "Masquer les rappels de sauvegarde", "Omitir recordatorios de guardado", "Nascondi i promemoria di salvataggio" }, "" },
    { "setting.hide_popup_save_reminder.tip", { "Enable to dismiss save reminders without saving your game.", "Schlie\u00dft Speichererinnerungen, ohne das Spiel zu speichern.", "Ferme les rappels de sauvegarde sans sauvegarder la partie.", "Cierra los recordatorios de guardado sin guardar la partida.", "Chiude i promemoria di salvataggio senza salvare la partita." }, "" },
    { "setting.hide_popup_villages.label", { "Disable Village Outcomes", "Dorfmeldungen ausblenden", "Masquer les nouvelles des villages", "Omitir resultados de aldeas", "Nascondi gli esiti dei villaggi" }, "" },
    { "setting.hide_popup_villages.tip", { "Enable to skip advisor notices after entering villages.", "Blendet Beratermeldungen nach dem Betreten von D\u00f6rfern aus.", "Masque les avis des conseillers apr\u00e8s l'entr\u00e9e dans un village.", "Omite los avisos de los asesores tras entrar en aldeas.", "Nasconde gli avvisi dei consiglieri dopo l'ingresso nei villaggi." }, "" },
    { "setting.hide_village_barbarian_messages.label", { "Disable Village Barbarian Messages", "Barbarenbelohnungen ausblenden", "Masquer les r\u00e9compenses barbares", "Omitir avisos de recompensas b\u00e1rbaras", "Nascondi gli avvisi dei premi barbari" }, "" },
    { "setting.hide_village_barbarian_messages.tip", { "Enable to skip Barbarian reward messages while still receiving the reward.", "Blendet Meldungen zu Barbarenbelohnungen aus. Die Belohnung wird trotzdem gew\u00e4hrt.", "Masque les messages de r\u00e9compense des Barbares. Vous recevez quand m\u00eame la r\u00e9compense.", "Omite los mensajes de recompensas de los B\u00e1rbaros. Sigues recibiendo la recompensa.", "Nasconde i messaggi delle ricompense dei Barbari. Ricevi comunque la ricompensa." }, "" },
    { "setting.hide_popup_city_captures.label", { "Disable City Capture Reports", "Stadteroberungen ausblenden", "Masquer les prises de villes", "Omitir informes de captura de ciudades", "Nascondi le conquiste delle citt\u00e0" }, "" },
    { "setting.hide_popup_city_captures.tip", { "Enable to skip notices when cities are taken or recaptured.", "Blendet Meldungen zu eroberten oder zur\u00fcckeroberten St\u00e4dten aus.", "Masque les avis de prise ou de reconqu\u00eate des villes.", "Omite los avisos cuando se capturan o recuperan ciudades.", "Nasconde gli avvisi quando le citt\u00e0 vengono conquistate o riconquistate." }, "" },
    { "setting.hide_popup_rival_government.label", { "Disable Rival Government Changes", "Regierungswechsel der Rivalen aus", "Masquer les r\u00e9gimes des rivaux", "Omitir cambios de gobierno de rivales", "Nascondi i cambi di governo rivali" }, "" },
    { "setting.hide_popup_rival_government.tip", { "Enable to skip notices when other civilizations change government.", "Blendet Meldungen aus, wenn andere Zivilisationen ihre Regierungsform \u00e4ndern.", "Masque les avis de changement de r\u00e9gime des autres civilisations.", "Omite los avisos cuando otras civilizaciones cambian de gobierno.", "Nasconde gli avvisi quando altre civilt\u00e0 cambiano governo." }, "" },
    { "setting.hide_popup_artifacts.label", { "Disable Artifact and Ancient Site News", "Artefakt-/Fundst\u00e4ttenmeldungen aus", "Masquer reliques et sites antiques", "Omitir reliquias y lugares antiguos", "Nascondi manufatti e siti antichi" }, "" },
    { "setting.hide_popup_artifacts.tip", { "Enable to skip artifact rumors and ancient site discoveries.", "Blendet Ger\u00fcchte \u00fcber Artefakte und Meldungen zu entdeckten antiken St\u00e4tten aus.", "Masque les rumeurs de reliques et les d\u00e9couvertes de sites antiques.", "Omite los rumores sobre reliquias y los descubrimientos de lugares antiguos.", "Nasconde le voci sui manufatti e le scoperte di siti antichi." }, "" },
    { "setting.hide_popup_spy_reports.label", { "Disable Spy Reports", "Spionageberichte ausblenden", "Masquer les rapports d'espionnage", "Omitir informes de espionaje", "Nascondi i rapporti di spionaggio" }, "" },
    { "setting.hide_popup_spy_reports.tip", { "Enable to skip notices about stolen gold and kidnapped Great People.", "Blendet Meldungen zu gestohlenem Gold und entf\u00fchrten Gro\u00dfen Pers\u00f6nlichkeiten aus.", "Masque les avis d'or vol\u00e9 et de Personnages illustres enlev\u00e9s.", "Omite los avisos sobre oro robado y Grandes personajes secuestrados.", "Nasconde gli avvisi sull'oro rubato e sui Grandi Personaggi rapiti." }, "" },
    { "setting.hide_popup_victory_warnings.label", { "Disable Victory Race Warnings", "Siegwarnungen ausblenden", "Masquer les alertes de victoire", "Omitir alertas de victoria", "Nascondi gli avvisi di vittoria" }, "" },
    { "setting.hide_popup_victory_warnings.tip", { "Enable to skip warnings about rivals nearing victory.", "Blendet Warnungen vor einem bevorstehenden Sieg der Rivalen aus.", "Masque les alertes signalant qu'un adversaire approche de la victoire.", "Omite las alertas sobre rivales que se acercan a la victoria.", "Nasconde gli avvisi sui rivali prossimi alla vittoria." }, "" },
    { "setting.hide_popup_government_available.label", { "Disable New Government Prompt", "Neue Regierungsformen ausblenden", "Masquer les nouveaux r\u00e9gimes", "Omitir avisos de nuevos gobiernos", "Nascondi gli avvisi di nuovi governi" }, "" },
    { "setting.hide_popup_government_available.tip", { "Enable to keep your current government when another becomes available.", "Beh\u00e4lt die aktuelle Regierungsform bei, wenn eine weitere verf\u00fcgbar wird.", "Conserve votre r\u00e9gime actuel lorsqu'un autre devient disponible.", "Mantiene el gobierno actual cuando hay otro disponible.", "Mantiene il governo attuale quando ne diventa disponibile un altro." }, "" },
    { "setting.hide_barbarian_messages.label", { "Disable Barbarian Leader Messages", "Barbarenanf\u00fchrer ausblenden", "Masquer les messages du chef barbare", "Omitir mensajes del l\u00edder b\u00e1rbaro", "Nascondi i messaggi del capo barbaro" }, "" },
    { "setting.hide_barbarian_messages.tip", { "Enable to skip the Barbarian leader's greeting when you first meet a tribe.", "\u00dcberspringt die Begr\u00fc\u00dfung des Barbarenanf\u00fchrers beim ersten Kontakt mit einem Stamm.", "Passe les salutations du chef barbare lors de la premi\u00e8re rencontre avec une tribu.", "Omite el saludo del l\u00edder b\u00e1rbaro al conocer a una tribu.", "Salta il saluto del capo barbaro al primo incontro con una trib\u00f9." }, "" },
    { "setting.hide_popup_trophy_room.label", { "Disable Trophy Room Offer", "Troph\u00e4enhallen-Einladung ausblenden", "Masquer l'invitation aux troph\u00e9es", "Omitir invitaci\u00f3n a Sala de la gloria", "Nascondi l'invito alla Sala della gloria" }, "" },
    { "setting.hide_popup_trophy_room.tip", { "Enable to dismiss invitations to visit the Trophy Room.", "Schlie\u00dft Einladungen zum Besuch der Troph\u00e4enhalle.", "Ferme les invitations \u00e0 visiter la Salle des troph\u00e9es.", "Cierra las invitaciones para visitar la Sala de la gloria.", "Chiude gli inviti a visitare la Sala della gloria." }, "" },
    { "setting.system_link_status.label", { "Connection Status", "Verbindungsstatus", "\u00c9tat de la connexion", "Estado de la conexi\u00f3n", "Stato della connessione" }, "" },
    { "setting.system_link_status.tip", { "Your current connection to the local network or relay.", "Aktueller Status der Verbindung zum lokalen Netzwerk oder Relay-Server.", "\u00c9tat actuel de la connexion au r\u00e9seau local ou au serveur relais.", "Estado actual de la conexi\u00f3n con la red local o el servidor intermediario.", "Stato attuale della connessione alla rete locale o al server relay." }, "" },
    { "setting.system_link_profile_name.label", { "Profile Name", "Profilname", "Nom de profil", "Nombre de perfil", "Nome del profilo" }, "" },
    { "setting.system_link_profile_name.tip", { "The name other players see.", "Der Name, den andere Spieler sehen.", "Le nom que voient les autres joueurs.", "El nombre que ven los dem\u00e1s jugadores.", "Il nome che vedono gli altri giocatori." }, "" },
    { "setting.system_link_invite.label", { "Invite Code", "Einladungscode", "Code d'invitation", "C\u00f3digo de invitaci\u00f3n", "Codice invito" }, "" },
    { "setting.system_link_invite.tip", { "Paste your host's invite code to join their relay room.", "Den Einladungscode des Gastgebers einf\u00fcgen, um dessen Relay-Raum beizutreten.", "Collez le code d'invitation de l'h\u00f4te pour rejoindre son salon relais.", "Pega el c\u00f3digo de invitaci\u00f3n del anfitri\u00f3n para unirte a su sala del servidor intermediario.", "Incolla il codice invito dell'host per entrare nella sua stanza relay." }, "" },
    { "setting.system_link_lan_enabled.label", { "Direct LAN", "Direktes LAN", "LAN direct", "LAN directa", "LAN diretta" }, "" },
    { "setting.system_link_lan_enabled.tip", { "Enable to play over your local network without a relay invite.", "Erm\u00f6glicht das Spielen im lokalen Netzwerk ohne Einladung \u00fcber einen Relay-Server.", "Permet de jouer sur votre r\u00e9seau local sans invitation via un serveur relais.", "Permite jugar en la red local sin una invitaci\u00f3n de un servidor intermediario.", "Permette di giocare sulla rete locale senza un invito tramite server relay." }, "" },
    { "setting.system_link_direct.label", { "Peer-to-Peer", "Direktverbindung", "Connexion directe", "Conexi\u00f3n directa", "Connessione diretta" }, "" },
    { "setting.system_link_direct.tip", { "Enable to connect directly to relay players when possible.", "Verbindet nach M\u00f6glichkeit direkt mit Spielern am Relay-Server.", "\u00c9tablit si possible une connexion directe avec les joueurs du serveur relais.", "Conecta directamente con los jugadores del servidor intermediario cuando es posible.", "Si connette direttamente ai giocatori del server relay quando possibile." }, "" },
    { "setting.live_sessions.label", { "Player Match Through Relay", "Mitspieler-Suche \u00fcber Relay", "Partie sans classement via relais", "Partida de jugador v\u00eda intermediario", "Partita del giocatore via relay" }, "" },
    { "setting.live_sessions.tip", { "Enable to host and find Player Match games in your relay room.", "Erm\u00f6glicht das Erstellen und Finden von Spielen der Mitspieler-Suche im eigenen Relay-Raum.", "Permet de cr\u00e9er et de trouver des parties sans classement dans votre salon relais.", "Permite crear y buscar partidas de jugador en tu sala del servidor intermediario.", "Permette di ospitare e cercare partite del giocatore nella tua stanza relay." }, "" },
    { "setting.discord_activity.label", { "Rich Presence", "Aktivit\u00e4tsanzeige", "Activit\u00e9 d\u00e9taill\u00e9e", "Actividad detallada", "Attivit\u00e0 dettagliata" }, "" },
    { "setting.discord_activity.tip", { "Enable to show what you're doing in ReRevved on Discord.", "Zeigt auf Discord an, was gerade in ReRevved gespielt wird.", "Affiche votre activit\u00e9 dans ReRevved sur Discord.", "Muestra en Discord lo que est\u00e1s haciendo en ReRevved.", "Mostra su Discord cosa stai facendo in ReRevved." }, "" },
    { "setting.discord_invites.label", { "Join Button", "Beitreten-Schaltfl\u00e4che", "Bouton Rejoindre", "Bot\u00f3n Unirse", "Pulsante Unisciti" }, "" },
    { "setting.discord_invites.tip", { "Enable to let Discord friends join your hosted Player Match.", "Erm\u00f6glicht Discord-Freunden den Beitritt zu selbst erstellten Spielen der Mitspieler-Suche.", "Permet \u00e0 vos amis Discord de rejoindre la partie sans classement que vous h\u00e9bergez.", "Permite a tus amigos de Discord unirse a la partida de jugador que organizas.", "Permette agli amici di Discord di unirsi alla partita del giocatore che ospiti." }, "" },
    { "setting.input_gamepad_in_background.label", { "Gamepad in Background", "Controller im Hintergrund", "Manette en arri\u00e8re-plan", "Mando en segundo plano", "Controller in background" }, "" },
    { "setting.input_gamepad_in_background.tip", { "Enable to keep the gamepad active while the window is inactive.", "L\u00e4sst den Controller auch bei inaktivem Spielfenster eingeschaltet.", "Maintient la manette active lorsque la fen\u00eatre du jeu est inactive.", "Mantiene el mando activo cuando la ventana del juego est\u00e1 inactiva.", "Mantiene attivo il controller quando la finestra del gioco non \u00e8 attiva." }, "" },
    { "setting.input_vibration.label", { "Vibration", "Vibration", "Vibration", "Vibraci\u00f3n", "Vibrazione" }, "" },
    { "setting.input_vibration.tip", { "Enable controller rumble.", "Aktiviert die Controller-Vibration.", "Active les vibrations de la manette.", "Activa la vibraci\u00f3n del mando.", "Attiva la vibrazione del controller." }, "" },
    { "setting.button_icons.label", { "Button Icons", "Tastensymbole", "Ic\u00f4nes des touches", "Iconos de botones", "Icone dei pulsanti" }, "" },
    { "setting.button_icons.tip", { "Choose which button icons the game shows.", "Legt fest, welche Tastensymbole das Spiel anzeigt.", "Choisit les ic\u00f4nes de touches affich\u00e9es dans le jeu.", "Elige los iconos de botones que muestra el juego.", "Scegli le icone dei pulsanti mostrate dal gioco." }, "" },
    { "setting.mnk_mode.label", { "Keyboard and Mouse", "Tastatur und Maus", "Clavier et souris", "Teclado y rat\u00f3n", "Tastiera e mouse" }, "" },
    { "setting.mnk_mode.tip", { "Enable to play with keyboard and mouse.", "Erm\u00f6glicht das Spielen mit Tastatur und Maus.", "Permet de jouer avec le clavier et la souris.", "Permite jugar con teclado y rat\u00f3n.", "Permette di giocare con tastiera e mouse." }, "" },
    { "setting.mnk_mouse.label", { "Mouse Aim", "Maussteuerung", "Vis\u00e9e \u00e0 la souris", "Apuntar con el rat\u00f3n", "Mira con il mouse" }, "" },
    { "setting.mnk_mouse.tip", { "Enable to move the right stick with your mouse.", "Erm\u00f6glicht die Steuerung des rechten Sticks mit der Maus.", "Permet de contr\u00f4ler le stick droit avec la souris.", "Permite controlar el stick derecho con el rat\u00f3n.", "Permette di controllare la levetta destra con il mouse." }, "" },
    { "setting.mnk_sensitivity.label", { "Mouse Sensitivity", "Mausempfindlichkeit", "Sensibilit\u00e9 de la souris", "Sensibilidad del rat\u00f3n", "Sensibilit\u00e0 del mouse" }, "" },
    { "setting.mnk_sensitivity.tip", { "How strongly mouse movement moves the right stick.", "Legt fest, wie stark Mausbewegungen auf den rechten Stick \u00fcbertragen werden.", "R\u00e8gle l'amplitude des mouvements du stick droit produits par la souris.", "Ajusta cu\u00e1nto se mueve el stick derecho al mover el rat\u00f3n.", "Regola quanto il movimento del mouse influisce sulla levetta destra." }, "" },
    { "setting.fullscreen.label", { "Fullscreen", "Vollbild", "Plein \u00e9cran", "Pantalla completa", "Schermo intero" }, "" },
    { "setting.fullscreen.tip", { "Enable to fill your selected monitor with the game.", "Zeigt das Spiel auf dem gew\u00e4hlten Monitor im Vollbild an.", "Affiche le jeu en plein \u00e9cran sur le moniteur choisi.", "Muestra el juego a pantalla completa en el monitor elegido.", "Mostra il gioco a schermo intero sul monitor selezionato." }, "" },
    { "setting.monitor.label", { "Monitor", "Monitor", "Moniteur", "Monitor", "Monitor" }, "" },
    { "setting.monitor.tip", { "Choose which monitor the game opens on.", "Legt fest, auf welchem Monitor das Spiel ge\u00f6ffnet wird.", "Choisit le moniteur sur lequel le jeu s'ouvre.", "Elige el monitor en el que se abre el juego.", "Scegli il monitor su cui aprire il gioco." }, "" },
    { "setting.window_size.label", { "Window Size", "Fenstergr\u00f6\u00dfe", "Taille de la fen\u00eatre", "Tama\u00f1o de ventana", "Dimensioni della finestra" }, "" },
    { "setting.window_size.tip", { "Choose the window size outside fullscreen.", "Legt die Fenstergr\u00f6\u00dfe au\u00dferhalb des Vollbildmodus fest.", "Choisit la taille de la fen\u00eatre hors du mode plein \u00e9cran.", "Elige el tama\u00f1o de la ventana fuera del modo de pantalla completa.", "Scegli le dimensioni della finestra quando non giochi a schermo intero." }, "" },
    { "setting.display_aspect.label", { "Aspect Ratio", "Seitenverh\u00e4ltnis", "Format d'image", "Relaci\u00f3n de aspecto", "Formato immagine" }, "" },
    { "setting.display_aspect.tip", { "Ultrawide shows more of the world on wide screens and keeps the interface centered.", "Ultrabreit zeigt auf breiten Bildschirmen mehr von der Welt und h\u00e4lt die Benutzeroberfl\u00e4che zentriert.", "Le format ultralarge montre plus du monde sur les \u00e9crans larges et garde l'interface centr\u00e9e.", "El formato ultrapanor\u00e1mico muestra m\u00e1s mundo en pantallas anchas y mantiene la interfaz centrada.", "Il formato ultrawide mostra pi\u00f9 mondo sugli schermi larghi e mantiene l'interfaccia al centro." }, "" },
    { "setting.resolution_scale.label", { "Render Resolution", "Renderaufl\u00f6sung", "R\u00e9solution de rendu", "Resoluci\u00f3n de renderizado", "Risoluzione di rendering" }, "" },
    { "setting.resolution_scale.tip", { "Choose how much detail the game renders. Auto matches your display.", "Legt den Detailgrad der Darstellung fest. Auto passt ihn an den Bildschirm an.", "Choisit le niveau de d\u00e9tail du rendu. Auto l'adapte \u00e0 votre \u00e9cran.", "Elige el nivel de detalle de la imagen. Auto lo adapta a tu pantalla.", "Scegli il livello di dettaglio dell'immagine. Auto lo adatta allo schermo." }, "" },
    { "setting.vsync.label", { "VSync", "VSync", "VSync", "VSync", "VSync" }, "" },
    { "setting.vsync.tip", { "Enable to keep the game at its normal frame pacing.", "H\u00e4lt bei Aktivierung die regul\u00e4ren Bildabst\u00e4nde des Spiels ein.", "Maintient la cadence d'affichage normale du jeu.", "Mantiene el ritmo normal de presentaci\u00f3n de fotogramas del juego.", "Mantiene la normale cadenza dei fotogrammi del gioco." }, "" },
    { "setting.present_effect.label", { "Upscaling", "Hochskalierung", "Mise \u00e0 l'\u00e9chelle", "Escalado", "Upscaling" }, "" },
    { "setting.present_effect.tip", { "Choose how the image is scaled and sharpened for your display.", "Legt fest, wie das Bild f\u00fcr den Bildschirm skaliert und gesch\u00e4rft wird.", "Choisit la mise \u00e0 l'\u00e9chelle et l'accentuation de l'image pour votre \u00e9cran.", "Elige c\u00f3mo se escala y se realza la nitidez de la imagen para tu pantalla.", "Scegli come adattare l'immagine allo schermo e aumentarne la nitidezza." }, "" },
    { "setting.swap_post_effect.label", { "Anti-Aliasing", "Kantengl\u00e4ttung", "Anticr\u00e9nelage", "Suavizado de bordes", "Antialiasing" }, "" },
    { "setting.swap_post_effect.tip", { "Choose how jagged edges are smoothed.", "Legt fest, wie gezackte Kanten gegl\u00e4ttet werden.", "Choisit la m\u00e9thode de lissage des contours en escalier.", "Elige c\u00f3mo se suavizan los bordes dentados.", "Scegli come smussare i bordi seghettati." }, "" },
    { "setting.audio_volume.label", { "Volume", "Lautst\u00e4rke", "Volume", "Volumen", "Volume" }, "" },
    { "setting.audio_volume.tip", { "Adjust how loudly the game plays.", "Regelt die Gesamtlautst\u00e4rke des Spiels.", "R\u00e8gle le volume sonore du jeu.", "Ajusta el volumen del juego.", "Regola il volume del gioco." }, "" },
    { "setting.audio_mute.label", { "Mute", "Stummschalten", "Couper le son", "Silenciar", "Disattiva audio" }, "" },
    { "setting.audio_mute.tip", { "Enable to silence all game audio.", "Schaltet s\u00e4mtliche Spielger\u00e4usche stumm.", "Coupe tous les sons du jeu.", "Silencia todo el sonido del juego.", "Disattiva tutti i suoni del gioco." }, "" },
    { "setting.audio_play_in_background.label", { "Play Sound in Background", "Ton im Hintergrund", "Son en arri\u00e8re-plan", "Sonido en segundo plano", "Audio in background" }, "" },
    { "setting.audio_play_in_background.tip", { "Enable to keep hearing the game while another window is active.", "Gibt den Spielton auch dann wieder, wenn ein anderes Fenster aktiv ist.", "Maintient le son du jeu lorsqu'une autre fen\u00eatre est active.", "Mantiene el sonido del juego cuando otra ventana est\u00e1 activa.", "Mantiene l'audio del gioco quando \u00e8 attiva un'altra finestra." }, "" },
    { "setting.retail_music_volume.label", { "Music Volume", "Musiklautst\u00e4rke", "Volume de la musique", "Volumen de la m\u00fasica", "Volume musica" }, "" },
    { "setting.retail_music_volume.tip", { "Set the music volume from 0 to 9. The game applies your choice each time a profile loads.", "Regelt die Musiklautst\u00e4rke von 0 bis 9. Die Einstellung wird bei jedem Laden eines Profils angewendet.", "R\u00e8gle le volume de la musique de 0 \u00e0 9. Le r\u00e9glage est appliqu\u00e9 \u00e0 chaque chargement d'un profil.", "Ajusta el volumen de la m\u00fasica de 0 a 9. Se aplica cada vez que se carga un perfil.", "Imposta il volume della musica da 0 a 9. La scelta viene applicata a ogni caricamento di un profilo." }, "" },
    { "setting.retail_effects_volume.label", { "Effects Volume", "Effektlautst\u00e4rke", "Volume des effets", "Volumen de los efectos", "Volume effetti" }, "" },
    { "setting.retail_effects_volume.tip", { "Set the sound effects volume from 0 to 9. The game applies your choice each time a profile loads.", "Regelt die Effektlautst\u00e4rke von 0 bis 9. Die Einstellung wird bei jedem Laden eines Profils angewendet.", "R\u00e8gle le volume des effets sonores de 0 \u00e0 9. Le r\u00e9glage est appliqu\u00e9 \u00e0 chaque chargement d'un profil.", "Ajusta el volumen de los efectos de sonido de 0 a 9. Se aplica cada vez que se carga un perfil.", "Imposta il volume degli effetti sonori da 0 a 9. La scelta viene applicata a ogni caricamento di un profilo." }, "" },
    { "setting.retail_advisor_volume.label", { "Advisor Speech Volume", "Beraterlautst\u00e4rke", "Volume des voix des conseillers", "Volumen de voz de los asesores", "Volume voci dei consiglieri" }, "" },
    { "setting.retail_advisor_volume.tip", { "Set the advisor speech volume from 0 to 9. The game applies your choice each time a profile loads.", "Regelt die Beraterlautst\u00e4rke von 0 bis 9. Die Einstellung wird bei jedem Laden eines Profils angewendet.", "R\u00e8gle le volume des voix des conseillers de 0 \u00e0 9. Le r\u00e9glage est appliqu\u00e9 \u00e0 chaque chargement d'un profil.", "Ajusta el volumen de voz de los asesores de 0 a 9. Se aplica cada vez que se carga un perfil.", "Imposta il volume delle voci dei consiglieri da 0 a 9. La scelta viene applicata a ogni caricamento di un profilo." }, "" },
    { "setting.retail_leader_volume.label", { "Leader Speech Volume", "Herrscherlautst\u00e4rke", "Volume des voix des dirigeants", "Volumen de voz de los l\u00edderes", "Volume voci dei leader" }, "" },
    { "setting.retail_leader_volume.tip", { "Set the leader speech volume from 0 to 9. The game applies your choice each time a profile loads.", "Regelt die Herrscherlautst\u00e4rke von 0 bis 9. Die Einstellung wird bei jedem Laden eines Profils angewendet.", "R\u00e8gle le volume des voix des dirigeants de 0 \u00e0 9. Le r\u00e9glage est appliqu\u00e9 \u00e0 chaque chargement d'un profil.", "Ajusta el volumen de voz de los l\u00edderes de 0 a 9. Se aplica cada vez que se carga un perfil.", "Imposta il volume delle voci dei leader da 0 a 9. La scelta viene applicata a ogni caricamento di un profilo." }, "" },
    { "choice.game_language.1", { "English", "English", "English", "English", "English" }, "" },
    { "choice.game_language.3", { "Deutsch", "Deutsch", "Deutsch", "Deutsch", "Deutsch" }, "" },
    { "choice.game_language.4", { "Fran\u00e7ais", "Fran\u00e7ais", "Fran\u00e7ais", "Fran\u00e7ais", "Fran\u00e7ais" }, "" },
    { "choice.game_language.5", { "Espa\u00f1ol", "Espa\u00f1ol", "Espa\u00f1ol", "Espa\u00f1ol", "Espa\u00f1ol" }, "" },
    { "choice.game_language.6", { "Italiano", "Italiano", "Italiano", "Italiano", "Italiano" }, "" },
    { "choice.combat_speed.normal", { "Normal", "Normal", "Normale", "Normal", "Normale" }, "" },
    { "choice.combat_speed.fast", { "Fast", "Schnell", "Rapide", "R\u00e1pida", "Veloce" }, "" },
    { "choice.resources.0", { "Normal", "Normal", "Normales", "Normales", "Normali" }, "" },
    { "choice.resources.1", { "Balanced", "Ausgewogen", "\u00c9quilibr\u00e9es", "Equilibrados", "Bilanciate" }, "" },
    { "choice.resources.3", { "Legendary", "Legend\u00e4r", "L\u00e9gendaires", "Legendarios", "Leggendarie" }, "" },
    { "choice.domination_min_turns.10", { "10 Turns (3000 BC)", "10 R. (3000 v. Chr.)", "10 t. (3000 av. J.-C.)", "10 t. (3000 a. C.)", "10 t. (3000 a.C.)" }, "" },
    { "choice.domination_min_turns.5", { "5 Turns (3500 BC)", "5 R. (3500 v. Chr.)", "5 t. (3500 av. J.-C.)", "5 t. (3500 a. C.)", "5 t. (3500 a.C.)" }, "" },
    { "choice.ai_civilizations.0", { "None (Multiplayer)", "Keine (Mehrspieler)", "Aucune (multi)", "Ninguna (multi)", "Nessuna (multi)" }, "" },
    { "choice.ai_civilizations.4", { "4 (Default)", "4 (Standard)", "4 (par d\u00e9faut)", "4 (por defecto)", "4 (predefinito)" }, "" },
    { "choice.gotw_civilization.0", { "Roman", "R\u00f6misch", "Romaine", "Romana", "Romana" }, "" },
    { "choice.gotw_civilization.1", { "Egyptian", "\u00c4gyptisch", "\u00c9gyptienne", "Egipcia", "Egiziana" }, "" },
    { "choice.gotw_civilization.2", { "Greek", "Griechisch", "Grecque", "Griega", "Greca" }, "" },
    { "choice.gotw_civilization.3", { "Spanish", "Spanisch", "Espagnole", "Espa\u00f1ola", "Spagnola" }, "" },
    { "choice.gotw_civilization.4", { "German", "Deutsch", "Allemande", "Alemana", "Tedesca" }, "" },
    { "choice.gotw_civilization.5", { "Russian", "Russisch", "Russe", "Rusa", "Russa" }, "" },
    { "choice.gotw_civilization.6", { "Chinese", "Chinesisch", "Chinoise", "China", "Cinese" }, "" },
    { "choice.gotw_civilization.7", { "American", "Amerikanisch", "Am\u00e9ricaine", "Estadounidense", "Americana" }, "" },
    { "choice.gotw_civilization.8", { "Japanese", "Japanisch", "Japonaise", "Japonesa", "Giapponese" }, "" },
    { "choice.gotw_civilization.9", { "French", "Franz\u00f6sisch", "Fran\u00e7aise", "Francesa", "Francese" }, "" },
    { "choice.gotw_civilization.10", { "Indian", "Indisch", "Indienne", "India", "Indiana" }, "" },
    { "choice.gotw_civilization.11", { "Arabian", "Arabisch", "Arabe", "\u00c1rabe", "Araba" }, "" },
    { "choice.gotw_civilization.12", { "Aztec", "Aztekisch", "Azt\u00e8que", "Azteca", "Azteca" }, "" },
    { "choice.gotw_civilization.13", { "Zulu", "Zulu", "Zouloue", "Zul\u00fa", "Zulu" }, "" },
    { "choice.gotw_civilization.14", { "Mongolian", "Mongolisch", "Mongole", "Mongola", "Mongola" }, "" },
    { "choice.gotw_civilization.15", { "English", "Englisch", "Anglaise", "Inglesa", "Inglese" }, "" },
    { "choice.button_icons.auto", { "Auto", "Auto", "Auto", "Auto", "Auto" }, "" },
    { "choice.button_icons.keyboard", { "Keyboard", "Tastatur", "Clavier", "Teclado", "Tastiera" }, "" },
    { "choice.window_size.default", { "Default (1280x720)", "Standard (1280x720)", "D\u00e9faut (1280x720)", "Normal (1280x720)", "Base (1280x720)" }, "" },
    { "choice.window_size.maximized", { "Maximized", "Maximiert", "Agrandie", "Maximizada", "Ingrandita" }, "" },
    { "choice.display_aspect.auto", { "Ultrawide", "Ultrabreit", "Ultralarge", "Ultrapanor\u00e1mico", "Ultrawide" }, "" },
    { "choice.resolution_scale.0", { "Auto", "Auto", "Auto", "Auto", "Auto" }, "" },
    { "choice.present_effect.bilinear", { "Bilinear", "Bilinear", "Bilin\u00e9aire", "Bilineal", "Bilineare" }, "" },
    { "choice.swap_post_effect.none", { "Off", "Aus", "D\u00e9sactiv\u00e9e", "No", "No" }, "" },
    { "choice.swap_post_effect.fxaa_extreme", { "FXAA (Extreme)", "FXAA (Extrem)", "FXAA (Extr\u00eame)", "FXAA (Extremo)", "FXAA (Estremo)" }, "" },
    { "common.on", { "On", "Ein", "Activ\u00e9e", "S\u00ed", "S\u00ec" }, "Value of a switch row, centered in the value column (about 26 em wide)." },
    { "common.off", { "Off", "Aus", "D\u00e9sactiv\u00e9e", "No", "No" }, "Value of a switch row, centered in the value column (about 26 em wide)." },
    { "common.unavailable", { "Unavailable", "Nicht verf\u00fcgbar", "Indisponible", "No disponible", "Non disponibile" }, "Value of a game volume row before the game has a value to show." },
    { "common.open", { "Open", "\u00d6ffnen", "Ouvrir", "Abrir", "Apri" }, "Value of an action row (Open Log Folder), centered in the value column (about 26 em wide)." },
    { "common.set", { "Set", "Gesetzt", "D\u00e9fini", "Definido", "Impostato" }, "Value of the Invite Code row when a code is saved. The code itself is never shown." },
    { "common.not_set", { "Not set", "Nicht gesetzt", "Non d\u00e9fini", "Sin definir", "Non impostato" }, "Value of the Invite Code row when no code is saved." },
    { "choice.monitor.0", { "Default", "Standard", "Par d\u00e9faut", "Predeterminado", "Predefinito" }, "Value of the Monitor row for the default monitor. Other monitors show the system's own names." },
    { "choice.system_link_profile_name.default", { "Default", "Standard", "Par d\u00e9faut", "Predeterminado", "Predefinito" }, "Value of the Profile Name row while no name is set (the game picks one). Centered in the value column." },
    { "note.copied", { "Copied.", "Kopiert.", "Copi\u00e9.", "Copiado.", "Copiato." }, "Footer note after the Invite Code row's Copy button put the code on the clipboard. One line." },
    { "note.week_range", { "Enter a week from 1 to 2147483647.", "Gib eine Woche von 1 bis 2147483647 ein.", "Saisissez une semaine de 1 \u00e0 2147483647.", "Escribe una semana de 1 a 2147483647.", "Inserisci una settimana da 1 a 2147483647." }, "Note under the text entry when the Game of the Week number is empty, zero or too big. One line; keep the numbers." },
    { "section.keybinds", { "Keybinds", "Tastenbelegung", "Touches", "Teclas", "Tasti" }, "Subheading above the controller keys that keyboard and mouse mode maps. Inside the Controls tab." },
    { "section.overlay_hotkeys", { "Overlay Hotkeys", "Tastenk\u00fcrzel", "Raccourcis des menus", "Atajos de men\u00fas", "Scorciatoie dei menu" }, "Subheading above the keys that open the in-game screens (settings, mods, achievements, console). Inside the Controls tab." },
    { "common.bind_none", { "(none)", "(keine)", "(aucune)", "(ninguna)", "(nessuno)" }, "Value of a keybind row with no key. Centered in the value column." },
    { "common.bind_tip_controller", { "Used when Keyboard and Mouse is on. Select, then press a key; hold Shift, Ctrl or Alt for a combination.", "Gilt bei aktivem Modus Tastatur und Maus. Ausw\u00e4hlen, dann eine Taste dr\u00fccken; mit Umschalt, Strg oder Alt f\u00fcr eine Kombination.", "Utilis\u00e9e avec le mode clavier et souris. S\u00e9lectionnez, puis appuyez sur une touche ; maintenez Maj, Ctrl ou Alt pour une combinaison.", "Se usa con el modo de teclado y rat\u00f3n. Elige y pulsa una tecla; mant\u00e9n May\u00fas, Ctrl o Alt para una combinaci\u00f3n.", "Usata con la modalit\u00e0 tastiera e mouse. Seleziona, poi premi un tasto; tieni premuto Maiusc, Ctrl o Alt per una combinazione." }, "Tooltip of a controller keybind row. Up to 3 lines. Key names such as Shift stay as the keyboard shows them." },
    { "common.bind_tip_hotkey", { "Select, then press the new key.", "Ausw\u00e4hlen, dann die neue Taste dr\u00fccken.", "S\u00e9lectionnez, puis appuyez sur la nouvelle touche.", "Elige y pulsa la nueva tecla.", "Seleziona, poi premi il nuovo tasto." }, "Tooltip of an overlay hotkey row. One or two lines." },
    { "note.keybind_capture", { "Press a key. Esc cancels.", "Taste dr\u00fccken. Esc bricht ab.", "Appuyez sur une touche. \u00c9chap annule.", "Pulsa una tecla. Esc cancela.", "Premi un tasto. Esc annulla." }, "Footer note while a keybind row waits for its key. One line. Esc is the key's name." },
    { "note.bind_conflict_one", { "Also bound to another action.", "Auch einer anderen Aktion zugewiesen.", "Aussi attribu\u00e9e \u00e0 une autre action.", "Tambi\u00e9n asignada a otra acci\u00f3n.", "Assegnata anche a un'altra azione." }, "Footer note under a keybind row whose key another bind also uses. One line." },
    { "note.bind_conflict", { "Also bound to {0} other actions.", "Auch {0} anderen Aktionen zugewiesen.", "Aussi attribu\u00e9e \u00e0 {0} autres actions.", "Tambi\u00e9n asignada a otras {0} acciones.", "Assegnata anche ad altre {0} azioni." }, "Footer note under a keybind row whose key other binds also use. {0} is how many (2 or more). One line." },
    { "setting.keybind_a.label", { "A Button", "A-Taste", "Bouton A", "Bot\u00f3n A", "Pulsante A" }, "" },
    { "setting.keybind_b.label", { "B Button", "B-Taste", "Bouton B", "Bot\u00f3n B", "Pulsante B" }, "" },
    { "setting.keybind_x.label", { "X Button", "X-Taste", "Bouton X", "Bot\u00f3n X", "Pulsante X" }, "" },
    { "setting.keybind_y.label", { "Y Button", "Y-Taste", "Bouton Y", "Bot\u00f3n Y", "Pulsante Y" }, "" },
    { "setting.keybind_left_trigger.label", { "Left Trigger", "Linker Trigger", "G\u00e2chette gauche", "Gatillo izquierdo", "Grilletto sinistro" }, "" },
    { "setting.keybind_right_trigger.label", { "Right Trigger", "Rechter Trigger", "G\u00e2chette droite", "Gatillo derecho", "Grilletto destro" }, "" },
    { "setting.keybind_left_shoulder.label", { "Left Shoulder", "Linke Schultertaste", "Bumper gauche", "Bot\u00f3n superior izquierdo", "Dorsale sinistro" }, "" },
    { "setting.keybind_right_shoulder.label", { "Right Shoulder", "Rechte Schultertaste", "Bumper droit", "Bot\u00f3n superior derecho", "Dorsale destro" }, "" },
    { "setting.keybind_lstick_up.label", { "Left Stick Up", "Linker Stick hoch", "Joystick gauche haut", "Palanca izquierda arriba", "Levetta sinistra su" }, "" },
    { "setting.keybind_lstick_down.label", { "Left Stick Down", "Linker Stick runter", "Joystick gauche bas", "Palanca izquierda abajo", "Levetta sinistra gi\u00f9" }, "" },
    { "setting.keybind_lstick_left.label", { "Left Stick Left", "Linker Stick links", "Joystick gauche vers la gauche", "Palanca izquierda a la izquierda", "Levetta sinistra a sinistra" }, "" },
    { "setting.keybind_lstick_right.label", { "Left Stick Right", "Linker Stick rechts", "Joystick gauche vers la droite", "Palanca izquierda a la derecha", "Levetta sinistra a destra" }, "" },
    { "setting.keybind_lstick_press.label", { "Left Stick Press", "Linker Stick dr\u00fccken", "Clic joystick gauche", "Pulsar palanca izquierda", "Pressione levetta sinistra" }, "" },
    { "setting.keybind_rstick_up.label", { "Right Stick Up", "Rechter Stick hoch", "Joystick droit haut", "Palanca derecha arriba", "Levetta destra su" }, "" },
    { "setting.keybind_rstick_down.label", { "Right Stick Down", "Rechter Stick runter", "Joystick droit bas", "Palanca derecha abajo", "Levetta destra gi\u00f9" }, "" },
    { "setting.keybind_rstick_left.label", { "Right Stick Left", "Rechter Stick links", "Joystick droit vers la gauche", "Palanca derecha a la izquierda", "Levetta destra a sinistra" }, "" },
    { "setting.keybind_rstick_right.label", { "Right Stick Right", "Rechter Stick rechts", "Joystick droit vers la droite", "Palanca derecha a la derecha", "Levetta destra a destra" }, "" },
    { "setting.keybind_rstick_press.label", { "Right Stick Press", "Rechter Stick dr\u00fccken", "Clic joystick droit", "Pulsar palanca derecha", "Pressione levetta destra" }, "" },
    { "setting.keybind_dpad_up.label", { "D-Pad Up", "Steuerkreuz hoch", "Croix directionnelle haut", "Cruceta arriba", "Croce direzionale su" }, "" },
    { "setting.keybind_dpad_down.label", { "D-Pad Down", "Steuerkreuz runter", "Croix directionnelle bas", "Cruceta abajo", "Croce direzionale gi\u00f9" }, "" },
    { "setting.keybind_dpad_left.label", { "D-Pad Left", "Steuerkreuz links", "Croix directionnelle gauche", "Cruceta izquierda", "Croce direzionale sinistra" }, "" },
    { "setting.keybind_dpad_right.label", { "D-Pad Right", "Steuerkreuz rechts", "Croix directionnelle droite", "Cruceta derecha", "Croce direzionale destra" }, "" },
    { "setting.keybind_back.label", { "Back Button", "Zur\u00fcck-Taste", "Bouton Retour", "Bot\u00f3n Atr\u00e1s", "Pulsante Indietro" }, "" },
    { "setting.keybind_start.label", { "Start Button", "Start-Taste", "Bouton D\u00e9marrer", "Bot\u00f3n Inicio", "Pulsante Start" }, "" },
    { "setting.keybind_guide.label", { "Guide Button", "Guide-Taste", "Bouton Guide", "Bot\u00f3n Gu\u00eda", "Pulsante Guida" }, "" },
    { "setting.bind_console.label", { "Toggle Console Overlay", "Konsole ein-/ausblenden", "Afficher ou masquer la console", "Mostrar u ocultar la consola", "Mostra o nascondi la console" }, "" },
    { "setting.bind_settings.label", { "Toggle Settings Overlay", "Einstellungen ein-/ausblenden", "Afficher ou masquer les param\u00e8tres", "Mostrar u ocultar los ajustes", "Mostra o nascondi le impostazioni" }, "" },
    { "setting.bind_mod_manager.label", { "Toggle Mod Manager Overlay", "Mod-Verwaltung ein-/ausblenden", "Afficher ou masquer les mods", "Mostrar u ocultar los mods", "Mostra o nascondi le mod" }, "" },
    { "setting.bind_achievements.label", { "Toggle Achievements Overlay", "Erfolge ein-/ausblenden", "Afficher ou masquer les succ\u00e8s", "Mostrar u ocultar los logros", "Mostra o nascondi gli obiettivi" }, "" },
    { "setting.bind_fps_counter.label", { "Toggle FPS Counter", "FPS-Anzeige ein-/ausblenden", "Afficher ou masquer les IPS", "Mostrar u ocultar los FPS", "Mostra o nascondi gli FPS" }, "" },
    { "common.bind_or", { "or", "oder", "ou", "o", "o" }, "Joins two keys of one bind in a keybind row's value (A or B). A lowercase word." },
    { "note.bind_refused", { "That key cannot be bound. Press another key.", "Diese Taste kann nicht belegt werden. Andere Taste dr\u00fccken.", "Cette touche ne peut pas \u00eatre attribu\u00e9e. Appuyez sur une autre touche.", "Esa tecla no se puede asignar. Pulsa otra tecla.", "Questo tasto non pu\u00f2 essere assegnato. Premi un altro tasto." }, "Footer note while a keybind row waits and the key pressed has no name to bind. One line." },
    { "note.bind_icons", { "Button icons update after a restart.", "Die Tastensymbole \u00e4ndern sich nach einem Neustart.", "Les ic\u00f4nes de touches changent apr\u00e8s un red\u00e9marrage.", "Los iconos de botones cambian tras reiniciar.", "Le icone dei tasti cambiano dopo il riavvio." }, "Footer note under a controller keybind row: the new key works at once, the on-screen button icons follow a restart. One line." },
    { "note.restart", { "Takes effect after restart.", "Wird nach einem Neustart wirksam.", "Prend effet apr\u00e8s red\u00e9marrage.", "Se aplica tras reiniciar.", "Ha effetto dopo il riavvio." }, "Footer note under a row whose change needs a restart. One line." },
    { "note.next_game", { "Takes effect in the next game.", "Wird im n\u00e4chsten Spiel wirksam.", "Prend effet \u00e0 la prochaine partie.", "Se aplica en la pr\u00f3xima partida.", "Ha effetto nella prossima partita." }, "Footer note under a row whose change applies to the next game. One line." },
    { "note.unsaved", { "Changed for this session, but it could not be saved.", "F\u00fcr diese Sitzung ge\u00e4ndert, aber nicht gespeichert.", "Modifi\u00e9 pour cette session, mais non enregistr\u00e9.", "Cambiado para esta sesi\u00f3n, pero no se ha podido guardar.", "Modificato per questa sessione, ma non salvato." }, "Footer note after a change that could not be written to the config file. One line." },
    { "note.rejected", { "That value was not accepted.", "Dieser Wert wurde nicht angenommen.", "Cette valeur a \u00e9t\u00e9 refus\u00e9e.", "No se ha aceptado ese valor.", "Il valore non \u00e8 stato accettato." }, "Footer note after a change the settings registry refused. One line." },
    { "note.no_monitor", { "Monitors are unavailable.", "Keine Monitore verf\u00fcgbar.", "Aucun moniteur disponible.", "No hay monitores disponibles.", "Nessun monitor disponibile." }, "Footer note when the Monitor row has no other monitor to pick. One line." },
    { "note.no_audio", { "Available once a game profile is loaded.", "Nach dem Laden eines Spielprofils verf\u00fcgbar.", "Disponible apr\u00e8s le chargement d'un profil de jeu.", "Disponible al cargar un perfil de juego.", "Disponibile dopo aver caricato un profilo di gioco." }, "Footer note under a game volume row before a profile is loaded. One line." },
});

// Windows-1252 for U+0080..U+009F; the rest of 0x80..0x9F is undefined.
struct CodePoint1252
{
    uint32_t codePoint;
    uint8_t  byte;
};

constexpr std::array<CodePoint1252, 27> kSpecials{ {
    { 0x20AC, 0x80 },
    { 0x201A, 0x82 },
    { 0x0192, 0x83 },
    { 0x201E, 0x84 },
    { 0x2026, 0x85 },
    { 0x2020, 0x86 },
    { 0x2021, 0x87 },
    { 0x02C6, 0x88 },
    { 0x2030, 0x89 },
    { 0x0160, 0x8A },
    { 0x2039, 0x8B },
    { 0x0152, 0x8C },
    { 0x017D, 0x8E },
    { 0x2018, 0x91 },
    { 0x2019, 0x92 },
    { 0x201C, 0x93 },
    { 0x201D, 0x94 },
    { 0x2022, 0x95 },
    { 0x2013, 0x96 },
    { 0x2014, 0x97 },
    { 0x02DC, 0x98 },
    { 0x2122, 0x99 },
    { 0x0161, 0x9A },
    { 0x203A, 0x9B },
    { 0x0153, 0x9C },
    { 0x017E, 0x9E },
    { 0x0178, 0x9F },
} };

constexpr char kReplacement = '?';

bool isContinuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

} // namespace

Language LanguageForXboxId(uint32_t xboxId) noexcept
{
    switch (xboxId)
    {
        case 3:
            return Language::kGerman;
        case 4:
            return Language::kFrench;
        case 5:
            return Language::kSpanish;
        case 6:
            return Language::kItalian;
        default:
            return Language::kEnglish;
    }
}

std::string_view Entry(Id id, Language language) noexcept
{
    const auto row    = static_cast<size_t>(id);
    const auto column = static_cast<size_t>(language);
    if (row >= kIdCount || column >= kLanguageCount)
        return {};
    return kIdRows[row].text[column];
}

std::string_view Localized(Id id, Language language) noexcept
{
    const auto entry = Entry(id, language);
    return entry.empty() ? Entry(id, Language::kEnglish) : entry;
}

size_t RowCount() noexcept
{
    return kIdCount + kKeyedRows.size();
}

const Row& RowAt(size_t index) noexcept
{
    return index < kIdCount ? kIdRows[index] : kKeyedRows[index - kIdCount];
}

const Row* FindRow(std::string_view key) noexcept
{
    static const std::unordered_map<std::string_view, const Row*> index = []
    {
        std::unordered_map<std::string_view, const Row*> map;
        for (size_t i = 0; i < RowCount(); ++i)
            map.emplace(RowAt(i).key, &RowAt(i));
        return map;
    }();
    const auto it = index.find(key);
    return it == index.end() ? nullptr : it->second;
}

std::string_view KeyOf(Id id) noexcept
{
    const auto row = static_cast<size_t>(id);
    return row < kIdCount ? kIdRows[row].key : std::string_view{};
}

namespace
{

std::string lowerKey(std::string_view name)
{
    std::string key;
    for (const char c : name)
        key.push_back(c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return key;
}

} // namespace

std::string TabKey(std::string_view name)
{
    return "tab." + lowerKey(name);
}

std::string SectionKey(std::string_view name)
{
    return "section." + lowerKey(name);
}

std::string SettingKey(std::string_view cvar, std::string_view field)
{
    return "setting." + std::string(cvar) + "." + std::string(field);
}

std::string ChoiceKey(std::string_view cvar, std::string_view raw)
{
    return "choice." + std::string(cvar) + "." + (raw.empty() ? std::string("default") : std::string(raw));
}

std::string Screen(std::string_view key, std::string_view english, Language language)
{
    if (language != Language::kEnglish)
    {
        if (const Row* row = FindRow(key))
        {
            const auto text = row->text[static_cast<size_t>(language)];
            if (!text.empty())
                return std::string(text);
        }
    }
    return std::string(english);
}

std::string Text(Id id, uint32_t xboxId)
{
    return ToWindows1252(Localized(id, LanguageForXboxId(xboxId)));
}

std::string ToWindows1252(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size())
    {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        size_t     length;
        uint32_t   codePoint;
        if (lead < 0x80)
        {
            length    = 1;
            codePoint = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            length    = 2;
            codePoint = lead & 0x1Fu;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            length    = 3;
            codePoint = lead & 0x0Fu;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            length    = 4;
            codePoint = lead & 0x07u;
        }
        else
        {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        bool valid = i + length <= utf8.size();
        for (size_t k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            valid           = isContinuation(next);
            codePoint       = codePoint << 6 | (next & 0x3Fu);
        }
        if (!valid)
        {
            out.push_back(kReplacement);
            ++i;
            while (i < utf8.size() && isContinuation(static_cast<unsigned char>(utf8[i])))
                ++i;
            continue;
        }
        i += length;
        constexpr std::array<uint32_t, 5> kMinimum{ 0, 0, 0x80, 0x800, 0x10000 };
        if (codePoint < kMinimum[length])
        {
            out.push_back(kReplacement);
            continue;
        }
        if (codePoint < 0x80 || (codePoint >= 0xA0 && codePoint <= 0xFF))
        {
            out.push_back(static_cast<char>(codePoint));
            continue;
        }
        char mapped = kReplacement;
        for (const auto& special : kSpecials)
        {
            if (special.codePoint == codePoint)
            {
                mapped = static_cast<char>(special.byte);
                break;
            }
        }
        out.push_back(mapped);
    }
    return out;
}

} // namespace rerevved::title_text

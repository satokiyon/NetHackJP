NOTICE: Modified by NetHackJP contributor @satokiyon; latest change date: 2026-09-02.
NetHack JP README


参考 : https://nethack.org/v500/ports/download-win.html

----

# Windows版 NetHack起動手順

1. ダウンロードしたzipファイルをすべて展開してください。
   (zipファイルの中にある状態で実行ファイルを起動しないでください)


2. 次のどちらかのファイルを起動してください。

   - NetHack.exe  (コンソール版)

   - NetHackW.exe (GUI版)


3. 必要であれば、ご自身の環境に合わせてNetHackの設定ファイルを編集してください。
   NetHackに関連するフォルダの場所と名前は、次のコマンドで確認できます。
   
     NetHack.exe --showpath

   設定は「.nethackrc」を編集してください。一度NetHackを起動すると
   nethackrc.template からコピーして自動作成されます。
   作成された %USERPROFILE%\NetHackJP\.nethackrc を編集してください。

----

# Linux版 NetHack起動手順

1. ダウンロードしたzipファイルをすべて展開してください。

     unzip NetHackJP-5.0.0-*-linux.zip


2. 展開されたディレクトリに移動し、必要に応じて実行権限が付与されているか確認してください。

     cd NetHackJP-5.0.0-*-linux
     chmod +x nethack nethack.bin


3. 以下のコマンドで起動してください（TTY / ncurses インターフェース）。

     ./nethack

   X11 GUI モードで起動する場合は以下を実行します（黒背景・白文字・白ボーダー枠線指定: `-bg black -fg white -bd white`）。

     ./nethack -wX11 -bg black -fg white -bd white

   【重要】X11 GUI 使用時に日本語が白四角（豆腐文字）で表示される場合は、Linux / WSL 環境に日本語 CJK フォントをインストールしてください：

     sudo apt update && sudo apt install -y fonts-noto-cjk fonts-ipafont-gothic
     fc-cache -fv


4. 必要であれば、ご自身の環境に合わせてNetHackの設定ファイルを編集してください。
   NetHackに関連するフォルダの場所と名前は、次のコマンドで確認できます。

     ./nethack --showpath

   設定は「.nethackrc」を編集してください。一度NetHackを起動すると
   nethackrc.template からコピーして自動作成されます。
   作成された $HOME/.nethackrc を編集してください。

----
     
設定ファイルに指定できるオプションや、ゲーム内容に関する説明は、Guidebook_JP.txtを参照してください。

Android版をプレイしたい場合はGoogle PlayのDartHackのページ( https://play.google.com/store/apps/details?id=jp.satokiyo.darthack )からインストールしてください。

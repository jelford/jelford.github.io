all: index.html

%.html : %.md
	pandoc --email-obfuscation=javascript --self-contained --css=style.css --standalone $< -f markdown -t html5 -o $@

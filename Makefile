
tl_sources := $(wildcard *.md)
tl_objects := $(patsubst %.md,%.html,$(tl_sources))
blog_sources := $(wildcard blog/*.md)
blog_objects := $(patsubst %.md,%.html,$(blog_sources))

.PHONY : blog clean

all: $(tl_objects) blog

$(tl_objects) : $(tl_sources)
	pandoc --email-obfuscation=javascript --self-contained --css=style.css --standalone $< -f markdown -t html5 -o $@

blog : $(blog_objects)
	echo '<html><head><link rel="stylesheet" type="text/css" href="style.css" ></head><body>' > blog.html
	for b in $(blog_objects); do cat $$b >> blog.html; done
	echo '</body></html>' >> blog.html
	

$(blog_objects) : $(blog_sources)
	pandoc --email-obfuscation=javascript $< -f markdown -t html5 -o $@


clean :
	find . -name '*.html' -exec rm {} \;


tl_sources := $(wildcard *.md)
tl_objects := $(patsubst %.md,%.html,$(tl_sources))
blog_sources := $(wildcard blog/*.md)
blog_objects := $(patsubst %.md,%.html,$(blog_sources))

.PHONY : blog clean

all : $(tl_objects) blog.html

$(tl_objects) : $(tl_sources)
	pandoc --email-obfuscation=javascript --self-contained --css=style.css --standalone $< -f markdown -t html5 -o $@

blog.html : $(blog_objects) compile_blog
	./compile_blog blog.html $(blog_objects)

blog/%.html : blog/%.md	
	pandoc --email-obfuscation=javascript $< -f markdown -t html5 -o $@

clean : 
	find . -name '*.html' -exec rm {} \;

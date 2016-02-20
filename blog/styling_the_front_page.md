% Bringing things together as a blog

So far we've taken a simple approach to gluing everyting together, but it'd be
good to have a couple of the niceties that make things seem like more than just...
a wall of text on a page.

I can think of a couple of nice features that would make things look better:

* Whole-page styling, e.g. link back to homepage
* Permalinks for individual posts
* Meta information on the posts (e.g. published date)

I still want to actually _write_ all the posts in simple markdown, but to get
e.g. permalinks, we're going to need the blog posts to make sense as
standalone HTML pages, and then when we bring them together in the front page,
we'll need to strip out just the content. First thing: we'll have pandoc
translate individual blog entries into standalone HTML pages

~~~
blog/%.html : blog/%.md	
	pandoc --email-obfuscation=javascript --self-contained --css=style.css --standalone $< -f markdown -t html5 -o $@
~~~

The `--standalone` argument instructs pandoc to make a full HTML file
(complete with headers, meta, and so on), while the `--self-contained` flag
instructs it to inline all the CSS. We don't necessarily want that in there
permanently, but if you try to build without it, pandoc with get the path
wrong (`style.css` sits in the root, but pandoc has no way to know that the
output document will be within a folder; it uses the relative path from the
current working directory).

That's enough to get us a reasonable output if we browse to the individual
blog posts by name, but there are a few things left:

* There's no way to browse to the individual blog posts; we still don't have
any kind of permalink to them.
* `compile_blog` is still sucking in the entire content of the blog posts
into the front page. Whilst modern browsers can figure out what's sensible,
it means we're left with  duplicate `<head>` blocks, css getting included
several times, and so on.

Let's address the first point first - it should be easy enough to add a link
in the body of every blog post to its own path.

## Adding Permalinks

So far, we've just been using pandoc's default HTML template, plus a
very simple stylesheet. The default HTML template has some useful bits in it;
it gives us a `<head>` section with some reasonable `<meta>` tags, and if we
take the time to inspect it, we'll see there's also some sort of shim to make
things nicer for users with an older version of internet explorer.

It's time to replace it with our own template, which will be even simpler:

* We won't bother with the `<meta>` tags for now - maybe we'll come back later
when we explicitly want them as a feature, and think about how we might want
them populated
* We won't bother with an HTML5 shim anymore; internet explorer's on version
11 now, so hopefully there aren't too many people out there still on version 8.

We do at least still want CSS to be passed in, so we'll make our new template
with reference to the default. You can see that by running:

~~~
pandoc -D html
~~~

There's some stuff in there we might want about authors, and it looks like
it'll try to generate some headers, title, and so on for us. That's all very
nice, but to begin with let's keep things super-simple and strip it right
down:

~~~
<!DOCTYPE html>
<html lang="en">
<head>
        <meta charset="utf-8" />
$for(css)$
        <link rel="stylesheet" href="$css$" type="text/css" />
$endfor$
</head>
<body>
<article>
$body$
</article>
</body>
</html>
~~~

What've we got here?

* A simple HTML 5 doctype (a simple "`html`" is all we need)
* Any CSS specified on the commandline will get added in.
* It's wrapped in an `<article>` tag because it's an 
(article)[https://www.w3.org/TR/html5/sections.html#the-article-element]. 
It's good to remember that our markup has semantic value. 

Now we've got a working standalone page that we can add to, let's get to adding
in our permalinks. We'll modify the body section of our template to include a
link to its "canonical" location:

~~~
<body>
<div><a href="$permalink$">permalink</a></div>
$body$
</body>
~~~

If let's put our new template in a file somewhere, and tell pandoc to use that
instead of the default. The template file:

~~~
cat > blog/pandoc_html_template.html.template << EOF
<!DOCTYPE html>
<html lang="en">
<head>
	<meta charset="utf-8" />
	<meta name="generator" content="pandoc" />
$for(css)$
	<link rel="stylesheet" href="$css$" type="text/css" />
$endfor$
</head>
<body>
<article>
<div><a href="$permalink$">permalink</a></div>
$body$
</article>
</body>
</html>
EOF
~~~

And then we update our Makefile:

~~~
blog/%.html : blog/%.md	pandoc_html_template.html.template
	pandoc --email-obfuscation=javascript --self-contained --css=style.css --standalone --template=blog/pandoc_html_template.html.template $< -f markdown -t html5 -o $@
~~~

Notice we also added the template to the list of dependencies for each blog
entry's HTML output. This ensures that when we change the template, our make
script will pick up the changes and know that it needs to regenerate _every_
blog post.

Let's give it a whirl, and see what we get:

<div style="border: 1px solid grey">
<a href="">permalink</a>
<h1>Bringing things together as a blog</h1>
<div>...</div>
</div>

Ah, that doesn't look to good - the link sitting above the title. Darn. Looks
like we might want to do something better with how we work with titles, so we
can put content below them in our templates. Taking another look at pandoc's
default template, it incldes the following lines:

~~~
$if(title)$
<div id="$idprefix$header">
<h1 class="title">$title$</h1>
...
~~~

The "title" in the case comes from metadata at the top of the file, which
pandoc reads in, and then makes available in the template rendering context.
You can get the details on what kinds of things we might want to put in meta-
data blocks from the pandoc 
[documentation](http://pandoc.org/README.html#metadata-blocks). We're
interested in the `% title` element. So far, I've been putting an H element at
the top of every blog post, by just writing:

~~~
% Bringing things together as a blog
...
~~~

Maybe it's time to _tell pandoc_ what the title of the post is, and then let it
figure out how to render the titles. So, as part of adding this feature, I've
gone through and replaced all the headers with metadata blogs; the lines above
become:

~~~
% Brining things together as a blog
~~~

and our template body changes to:

~~~
<body>
<article>
<h1 class="title">$title$</h1>
<a href="">permalink</a>
$body$
</article>
</body>
~~~

I'm not going to go though manually changing the first line of every blog post;
I'll just run:

~~~
find blog -name '*.md' -exec sed -e 's/^# /% /g' {} \;
~~~

to get all the `<h1>` elements.

Finally, we're still missing a crucial detail - all our permalinks are missing
their `href` attributes (or rather, they're all blank). We need to pass in the
relative file path to the template when we generate the page. That's one last
tweak to our Makefile:

~~~
pandoc --email-obfuscation=javascript --self-contained --css=style.css --standalone --template=blog/pandoc_html_template.html.template -V permalink=/$@ $< -f markdown -t html5 -o $@
~~~

Hey presto - we get html that looks like this:

~~~
<body>
<article>
<h1>...</h1>
<a class="permalink" href="blog/styling_the_front_page.html">permalink</a>
<p> ...
</article>
~~~

## Tidying up the main page

Now that we've made the changes we needed to add permalinks, it's time we clear
up the main page. Let's stop the `compile_blog` step from pulling in extraneous
HTML from the sub pages, for a start. The approach I'm going to take is to
parse sub-pages and extract just their `body` elements. It's easy to do with
the very adequate [Beautiful Soup](http://www.crummy.com/software/BeautifulSoup/)
library. This is the first piece of third-party software I'm using so far for
this project - I'll start a local [virtualenv](http://virtualenv.readthedocs.org/en/latest/)
to keep my external dependencies tidily separated from the rest of my system:

~~~
sudo pip install --upgrade pip setuptools virtualenv wheel # update everything
virtualenv . --python=python3
. bin/activate
pip install --upgrade beautifulsoup4
~~~

I'll also add some entries to `.gitignore` to make that not be a huge pain.
Helpfully, there's a github project that collects together commonly used
`.gitignore` snippets [here](https://github.com/github/gitignore), or if you
prefer to have someone do your text-file-gluing-togther for you, there's a neat
wrapper around there [here](https://www.gitignore.io/).

One more thing - I'll want to check in a list of dependencies:

~~~
pip freeze > requirements.txt
git add requirements.txt
~~~

Now we'll need to do

~~~
. bin/activate
~~~

whenever we start working on the blog, to set up our working environment correctly.

Right, back to the business of sorting out the front page. We'll modify
`compile_blog` to extract just the `<article>` section of our sub-pages:

~~~
class BlogPost():
	...
	def content(self):
		with open(self.file_path, 'r') as f:
		 	soup = BeautifulSoup(f.read())
		return soup.body.article

~~~

`BlogPost.content()` now returns a beautifulsoup HTML element - let's update
the compile step to be aware of that, and apply any top-level styling:

~~~
def compile(...):
	...
	for b in reversed(sorted(blog_posts, key=lambda b: b.timestamp())):
		post = b.content()
		post['class'] = 'blog_post' # add styling that applies at the top level
		output_file.write(str(post))
~~~

## Styling the main page

So we're finally in a place where the main page has some reasonable markup on
it, embedding individual blog posts, that can be linked to as standalone pages.
Nice going. Finally, let's add some simple styling that links back to the
site's main page, with some borders, and then let's call it a day. We'll come
back and add in date information to individual posts next time maybe.

First, I'm just going to add a header to the top of every page. We'll modify
the front page as follows:

~~~
def compile(output_file, blog_posts):
	...
	output_file.write('<header><h1>jelford\'s blog</h1><nav><ul><li><a href="/">home</a></li><li><a href="https://github.com/jelford">github</a></li></ul></nav></header>')
~~~

That gives us the main links we'll want at the top, along with an originally
named header. One last thing - add the CSS to get the links to flow
horizontally:

~~~
nav ul {
	list-style-type: none;
	margin: 0;
	padding: 0;
}

nav ul li {
	display: inline;
	margin-right: 0.3em;
}
~~~

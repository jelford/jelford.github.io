% More styling

To make things not horrible, let's do a bit of styling:

* Get rid of the wall-of-text on large screens
* Format code segments a bit nicer
* Give the page a title so it looks nicer in the browser

## Resize the page according to screen size

We're not going to do anything very clever here - I just hate trying to read
pages that span the whole width of my screen with text. It's not that easy to
read (I still line-wrap at 80 characters in vim).

We can add the following to `style.css`:

~~~
body {
    ...
    margin-left: auto;
    margin-right: auto;
    max-width: 55em;
}
~~~

And we're done with widths. `max-width` won't have any effect on smaller
displays, but when we open it up on a widescreen monitor, the text will stay
within a reasonably widthed area, in the middle of the viewport.

## Prettify the code

pandoc supports code highlighting, to some extent, using the following syntax
([docs](http://pandoc.org/README.html#fenced-code-blocks)):

~~~~ { .markdown .sourcecode }
~~~ { .css }
...
~~~
~~~~

That generates HTML like:

~~~ { .html .sourcecode }
<tbody><tr class="sourceCode"><td class="lineNumbers"><pre>1
2
3
4
5
</pre></td><td class="sourceCode"><pre><code class="sourceCode css">pre code <span class="kw">{</span>
    <span class="kw">display:</span> <span class="dt">block</span><span class="kw">;</span>
    <span class="kw">background-color:</span> <span class="dt">#EEEEEE</span><span class="kw">;</span>
    <span class="kw">overflow-x:</span> <span class="dt">auto</span><span class="kw">;</span>
<span class="kw">}</span></code></pre></td></tr></tbody>
~~~

Unfortunately, this isn't actually super easy to style up in CSS - you're stuck
with table-based layout (if you enable line numbers), and we'll have to do
something about actually having some meaningful CSS to make the highlighting
look good. Okay, we'll try something else (not writing our own generic lexer) -
enter [highlight.js](https://highlightjs.org/). 

Now, I _do_ want to use the feature of pandoc where we can conveniently add
css classnames to our fenced code blocks, so we won't turn the extension off
entirely - but I'll add a `--no-highlight` line to my pandoc make tasks. That
leaves the task of adding the highlighting up to highlight.js. We want 
something like (pseudocode):

~~~ { .javascript .sourcecode }
var code = document.querySelectorAll('pre.sourcecode');
for (let block of code) {
	hljs.do_highlighting_please(...);
}
~~~

Since we're already thinking about doing our own initialization, we might as 
well take the time to put this work off the main thread; there's no need to
block the whole browser while we apply styling. Luckily the highlight.js
instructions come with a recipe for putting the work onto a worker thread:

~~~ { .javascript .sourcecode }
// in the main script:
addEventListener('load', function() {
  var code = document.querySelector('#code');
  var worker = new Worker('worker.js');
  worker.onmessage = function(event) { code.innerHTML = event.data; }
  worker.postMessage(code.textContent);
})

// in worker.js:
onmessage = function(event) {
  importScripts('<path>/highlight.pack.js');
  var result = self.hljs.highlightAuto(event.data);
  postMessage(result.value);
}
~~~

That won't quite do for us; we've got more than one code block - so we need to
adapt what's going on in the main script to handle that. We _could_ just change

~~~ { .javascript .sourcecode }
  var code = document.querySelector('#code');
  ...
~~~

to

~~~ { .javascript .sourcecode }
  var code = document.querySelectorAll('pre.sourcecode');
  for (let block of code) {
    ...
  }
~~~

but it's not necessarily fine to just spawn an infinite number of worker
threads. According to [mdn](https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Using_web_workers#About_thread_safety),
each worker spawns a real OS-level thread. Potentially, that's very expensive;
and we don't really need loads of work going on in parallel - we're just trying
to do some syntax highlighting!

Let's adapt the code so that we send all our requests to a single worker, and
when we get the results back, they have a key that lets us put them in the
correct code block:

~~~ { .javascript .sourcecode }
// in js/blog-highlight.js
document.addEventListener('DOMContentLoaded', function() {
	/* no .sourcecode anymore; turns our all our pres are code, so it's redundant */
	var code = document.querySelectorAll('pre'); 
	var worker = new Worker('js/highlight-worker.js');

	worker.onmessage = function(event) { 
		let target = code[event.data.sourceid].lastChild
		target.innerHTML = event.data.content; 
		target.classList.add('hljs');
	};

	// Can't pass the actual nodes to the workers, so use the list index as a kind of key
	for (let i=0; i<code.length; ++i) {
		worker.postMessage({
			content: code[i].lastChild.textContent, 
			sourceid: i
		});
	}
});


// in js/highlight-worker.js
importScripts('/js/highlight.pack.js');

onmessage = function(event) {
	console.log(event);
	var result = self.hljs.highlightAuto(event.data.content);
	postMessage({sourceid: event.data.sourceid, content: result.value});
}
~~~

We'll have to add `js/blog-highlight.js` to both the front page, and the
individual page templates. Once we've done that, there's just one more thing
bothering me; how does it look for people without javascript? (hint: not good).

We can do something about that though; let's script up a very simple code pane
style for clients without javascipt. In our HTML it'll look something like
this:

~~~ { .sourcecode .html }
<head>
	...
	<noscript>
		<link rel="stylesheet" type="text/css" href="/styles/noscript.css">
	</noscript>
	...
</head>
~~~

~~~ { .sourcecode .css }
pre code {
    background-color: #F0F0F0;
    display: block;
    padding: 5px;
    border-radius: 5px;
}
~~~

At that point, arguably, we're done pretifying the code.

## Adding a title

We already took the time to get pandoc to be aware of the page title (that's
what generates our `<h1>` tags at the top of each post). Let's use the same
capability to add `<title>` tags to our blog post `<head>s`.

In `blog/pandoc_html_template.html.template`:

~~~ { .sourcecode .html }
<head>
	...
	<title>$title$</title>
	...
</head>
~~~

We'd better also add a similar thing in the top-level page, but that'll say
something like:

~~~ { .sourcecode .python }
	output_file.write('<title>jelford\'s blog</title>')
~~~

That's all we need to get a decent title up at the top of the screen.

## Back links from blog entries

One final thing I didn't mention at the start; I'm going to add a nav bar at
the top of the individual blog entries. That makes it easier to get back to
reading the main page from inside a blog:

~~~ { .sourcecode .html }
<body>
	<header>
		<nav>
			<ul>
				<li><a href="/blog.html">blog</a></li>
				<li><a href="/">home</a></li>
				<li><a href="https://github.com/jelford">github</a></li>
			</ul>
		</nav>
	</header>
	<article>
		...
~~~

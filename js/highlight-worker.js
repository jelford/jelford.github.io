importScripts('/js/highlight.pack.js');

onmessage = function(event) {
	console.log(event);
	try {
		var result = self.hljs.highlightAuto(event.data.content);
		postMessage({sourceid: event.data.sourceid, content: result.value});
	} catch (e) {
		console.log(e);
	}
}

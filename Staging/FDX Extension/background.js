const HOST_NAME = "com.fahad.fdx";
let debounceCache = new Set();

chrome.runtime.onInstalled.addListener(() => {
    chrome.contextMenus.create({ id: "download_with_fdx", title: "Download with FDX", contexts: ["link", "video", "audio", "image"] });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
    if (info.menuItemId === "download_with_fdx") {
        let targetUrl = info.linkUrl || info.srcUrl || info.pageUrl;
        if (targetUrl) sendToFDX(targetUrl);
    }
});

chrome.downloads.onCreated.addListener((downloadItem) => {
    if (downloadItem.state !== 'in_progress') return; 
    
    // CRITICAL FIX: Chrome tries to resume old downloads when it starts.
    // If the download was started more than 10 seconds ago, IGNORE IT!
    let age = Date.now() - new Date(downloadItem.startTime).getTime();
    if (age > 10000) return;

    chrome.downloads.cancel(downloadItem.id);
    sendToFDX(downloadItem.url);
});

// Rock-Solid Movie Sniffer
chrome.webRequest.onResponseStarted.addListener(
    (details) => {
        if (details.tabId < 0) return;
        let url = details.url.toLowerCase();

        let isMovie = details.type === 'media' || url.includes('.m3u8') || url.includes('.mp4') || url.includes('.mkv') || url.includes('.ts');

        if (isMovie) {
            chrome.tabs.sendMessage(details.tabId, { 
                action: "show_media_panel", 
                url: details.url 
            }, { frameId: 0 }).catch(() => {});
        }
    },
    { urls: ["<all_urls>"] },
    ["responseHeaders"]
);

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request.action === "download_sniffed_media" && request.url) {
        sendToFDX(request.url);
    }
});

function sendToFDX(url) {
    if (!url || url.startsWith('chrome') || url.startsWith('blob') || url.startsWith('data')) return;
    if (debounceCache.has(url)) return; 
    
    debounceCache.add(url);
    setTimeout(() => debounceCache.delete(url), 2000); 

    chrome.runtime.sendNativeMessage(HOST_NAME, { command: "add_download", url: url }, (response) => {
        if (chrome.runtime.lastError) console.error("FDX Bridge Error:", chrome.runtime.lastError.message);
    });
}
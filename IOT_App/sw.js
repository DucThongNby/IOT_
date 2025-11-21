// Tên của cache
const CACHE_NAME = 'smart-home-pwa-v1';

// Các tệp cần cache để chạy offline
// Quan trọng: Phải đổi tên 'index.html' thành tên file HTML của bạn
const FILES_TO_CACHE = [
  'smart_home_app.html',
  'manifest.json'
  // Bạn có thể thêm các link CDN (Tailwind, Lucide) vào đây nếu muốn,
  // nhưng nó phức tạp hơn và cần xử lý CORS.
  // Hiện tại, chúng ta chỉ cache các file cục bộ.
];

// Cài đặt Service Worker và cache các tệp cơ bản
self.addEventListener('install', (event) => {
  console.log('SW: Đang cài đặt...');
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      console.log('SW: Đang cache các tệp cơ bản');
      // Thêm các tệp vào cache.
      // fetch() là cần thiết vì addAll() sẽ tự fetch các request.
      return cache.addAll(FILES_TO_CACHE.map(url => new Request(url, { cache: 'reload' })));
    })
  );
  self.skipWaiting(); // Kích hoạt SW ngay lập E.
});

// Kích hoạt Service Worker và dọn dẹp cache cũ
self.addEventListener('activate', (event) => {
  console.log('SW: Đang kích hoạt...');
  event.waitUntil(
    caches.keys().then((keyList) => {
      return Promise.all(keyList.map((key) => {
        if (key !== CACHE_NAME) {
          console.log('SW: Xóa cache cũ', key);
          return caches.delete(key);
        }
      }));
    })
  );
  return self.clients.claim(); // SW kiểm soát trang ngay lập E.
});

// Can thiệp vào các yêu cầu (fetch)
self.addEventListener('fetch', (event) => {
  // Chỉ xử lý các yêu cầu GET
  if (event.request.method !== 'GET') {
    return;
  }

  // Chiến lược: Ưu tiên cache (Cache First)
  // Cố gắng tìm tài nguyên trong cache trước.
  event.respondWith(
    caches.open(CACHE_NAME).then((cache) => {
      return cache.match(event.request).then((response) => {
        if (response) {
          // Nếu tìm thấy trong cache, trả về nó
          console.log(`SW: Lấy từ cache: ${event.request.url}`);
          return response;
        }

        // Nếu không tìm thấy, đi lấy từ mạng
        console.log(`SW: Lấy từ mạng: ${event.request.url}`);
        return fetch(event.request).then((networkResponse) => {
          // (Tùy chọn) Bạn có thể cache lại tài nguyên vừa lấy từ mạng ở đây
          // cache.put(event.request, networkResponse.clone());
          return networkResponse;
        });

      }).catch((error) => {
        console.error('SW: Lỗi fetch:', error);
        // Có thể trả về một trang offline dự phòng ở đây
      });
    })
  );
});

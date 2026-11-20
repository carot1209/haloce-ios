/*
HOST_HTTPS.M

The server browser's requests to the master servers (host_https_get,
port/android/guest/runtime/guest_host.h; port/linux/src/master_list.c), on
iOS: NSURLSession, with the system's TLS and certificate authorities, in
place of the Android host's Mbed TLS client. The guest's thread waits for
the answer, as it does on Android.
*/

#import <Foundation/Foundation.h>

#include "host.h"

#include <string.h>

int host_https_get(const char *url, int *status, char *headers, int headers_size, char *body, int body_capacity,
	int *body_size, char *error, int error_size)
{
	__block int ok = 0;

	*status = 0;
	*body_size = 0;
	if (headers_size > 0)
		headers[0] = 0;
	if (error_size > 0)
		error[0] = 0;
	@autoreleasepool
	{
		NSURL *address = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
		NSMutableURLRequest *request;
		dispatch_semaphore_t done = dispatch_semaphore_create(0);

		if (!address)
		{
			snprintf(error, (size_t)error_size, "not a URL: %s", url);
			return 0;
		}
		request = [NSMutableURLRequest requestWithURL:address cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
			timeoutInterval:15];
		request.HTTPMethod = @"GET";
		[[NSURLSession.sharedSession dataTaskWithRequest:request
			completionHandler:^(NSData *data, NSURLResponse *response, NSError *failure) {
				if (failure || ![response isKindOfClass:NSHTTPURLResponse.class])
				{
					snprintf(error, (size_t)error_size, "%s",
						failure ? failure.localizedDescription.UTF8String : "no HTTP response");
				}
				else
				{
					NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
					NSMutableString *lines = [NSMutableString string];
					int size = (int)MIN((NSUInteger)body_capacity, data.length);

					*status = (int)http.statusCode;
					[http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL *stop) {
						[lines appendFormat:@"%@: %@\r\n", key, value];
					}];
					snprintf(headers, (size_t)headers_size, "%s", lines.UTF8String);
					if (size > 0)
						memcpy(body, data.bytes, (size_t)size);
					*body_size = size;
					ok = 1;
				}
				dispatch_semaphore_signal(done);
			}] resume];
		dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	}
	return ok;
}

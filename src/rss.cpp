#include <rss.h>
#include <config.h>
#include <cache.h>
#include <tagsouppullparser.h>
#include <utils.h>
#include <logger.h>
#include <exceptions.h>
#include <sstream>
#include <iostream>
#include <configcontainer.h>
#include <cstring>
#include <algorithm>
#include <curl/curl.h>
#include <sys/utsname.h>
#include <htmlrenderer.h>

#include <langinfo.h>

#include <cerrno>

#include <functional>

using namespace newsbeuter;

rss_parser::rss_parser(const char * uri, cache * c, configcontainer * cfg, rss_ignores * ii) 
	: my_uri(uri), ch(c), cfgcont(cfg), mrss(0), ign(ii) { }

rss_parser::~rss_parser() { }

rss_feed rss_parser::parse() {
	rss_feed feed(ch);
	bool skip_parsing = false;

	feed.set_rssurl(my_uri);

	/*
	 * This is a bit messy.
	 *	- http:// and https:// URLs are downloaded and parsed regularly
	 *	- exec: URLs are executed and their output is parsed
	 *	- filter: URLs are downloaded, executed, and their output is parsed
	 *	- query: URLs are ignored
	 */
	mrss_error_t err;
	int my_errno = 0;
	CURLcode ccode = CURLE_OK;
	if (my_uri.substr(0,5) == "http:" || my_uri.substr(0,6) == "https:") {
		mrss_options_t * options = create_mrss_options();
		{
			scope_measure m1("mrss_parse_url_with_options_and_error");
			err = mrss_parse_url_with_options_and_error(const_cast<char *>(my_uri.c_str()), &mrss, options, &ccode);
		}
		my_errno = errno;
		GetLogger().log(LOG_DEBUG, "rss_parser::parse: http URL, err = %u errno = %u (%s)", err, my_errno, strerror(my_errno));
		mrss_options_free(options);
	} else if (my_uri.substr(0,5) == "exec:") {
		std::string file = my_uri.substr(5,my_uri.length()-5);
		std::string buf = utils::get_command_output(file);
		GetLogger().log(LOG_DEBUG, "rss_parser::parse: output of `%s' is: %s", file.c_str(), buf.c_str());
		err = mrss_parse_buffer(const_cast<char *>(buf.c_str()), buf.length(), &mrss);
		my_errno = errno;
	} else if (my_uri.substr(0,7) == "filter:") {
		std::string filter, url;
		utils::extract_filter(my_uri, filter, url);
		std::string buf = utils::retrieve_url(url, utils::get_useragent(cfgcont).c_str());

		char * argv[2];
		argv[0] = const_cast<char *>(filter.c_str());
		argv[1] = NULL;
		std::string result = utils::run_program(argv, buf);
		GetLogger().log(LOG_DEBUG, "rss_parser::parse: output of `%s' is: %s", filter.c_str(), result.c_str());
		err = mrss_parse_buffer(const_cast<char *>(result.c_str()), result.length(), &mrss);
		my_errno = errno;
	} else if (my_uri.substr(0,6) == "query:") {
		skip_parsing = true;
		err = MRSS_OK;
	} else {
		throw utils::strprintf(_("Error: unsupported URL: %s"), my_uri.c_str());
	}

	if (!skip_parsing) {

		if (!mrss)
			return feed;

		if (err > MRSS_OK && err <= MRSS_ERR_DATA) {
			if (err == MRSS_ERR_POSIX) {
				GetLogger().log(LOG_ERROR,"rss_parser::parse: mrss_parse_* failed with POSIX error: error = %s",strerror(my_errno));
			}
			GetLogger().log(LOG_ERROR,"rss_parser::parse: mrss_parse_* failed: err = %s (%u %x)",mrss_strerror(err), err, err);
			GetLogger().log(LOG_ERROR,"rss_parser::parse: CURLcode = %u (%s)", ccode, curl_easy_strerror(ccode));
			GetLogger().log(LOG_DEBUG,"rss_parser::parse: saved errno = %d (%s)", my_errno, strerror(my_errno));
			GetLogger().log(LOG_USERERROR, "RSS feed `%s' couldn't be parsed: %s (error %u)", my_uri.c_str(), mrss_strerror(err), err);
			if (mrss) {
				mrss_free(mrss);
			}
			throw std::string(mrss_strerror(err));
		}

		/*
		 * After parsing is done, we fill our feed object with title,
		 * description, etc.  It's important to note that all data that comes
		 * from mrss must be converted to UTF-8 before, because all data is
		 * internally stored as UTF-8, and converted on-the-fly in case some
		 * other encoding is required. This is because UTF-8 can hold all
		 * available Unicode characters, unlike other non-Unicode encodings.
		 */

		const char * encoding = mrss->encoding ? mrss->encoding : "utf-8";

		if (mrss->title) {
			if (mrss->title_type && (strcmp(mrss->title_type,"xhtml")==0 || strcmp(mrss->title_type,"html")==0)) {
				std::string xhtmltitle = utils::convert_text(mrss->title, "utf-8", encoding);
				feed.set_title(render_xhtml_title(xhtmltitle, feed.link()));
			} else {
				feed.set_title(utils::convert_text(mrss->title, "utf-8", encoding));
			}
		}
		
		if (mrss->description) {
			feed.set_description(utils::convert_text(mrss->description, "utf-8", encoding));
		}

		if (mrss->link) {
			feed.set_link(utils::absolute_url(my_uri, mrss->link));
		}

		if (mrss->pubDate) 
			feed.set_pubDate(parse_date(mrss->pubDate));
		else
			feed.set_pubDate(::time(NULL));

		// we implement right-to-left support for the languages listed in
		// http://blogs.msdn.com/rssteam/archive/2007/05/17/reading-feeds-in-right-to-left-order.aspx
		if (mrss->language) {
			static const char * rtl_langprefix[] = { 
				"ar",  // Arabic
				"fa",  // Farsi
				"ur",  // Urdu
				"ps",  // Pashtu
				"syr", // Syriac
				"dv",  // Divehi
				"he",  // Hebrew
				"yi",  // Yiddish
				NULL };
			for (unsigned int i=0;rtl_langprefix[i]!=NULL;++i) {
				if (strncmp(mrss->language,rtl_langprefix[i],strlen(rtl_langprefix[i]))==0) {
					GetLogger().log(LOG_DEBUG, "rss_parser::parse: detected right-to-left order, language code = %s", rtl_langprefix[i]);
					feed.set_rtl(true);
					break;
				}
			}
		}

		GetLogger().log(LOG_DEBUG, "rss_parser::parse: feed title = `%s' link = `%s'", feed.title().c_str(), feed.link().c_str());

		/*
		 * Then we iterate over all items. Each item is filled with title, description,
		 * etc. and is then appended to the feed.
		 */
		for (mrss_item_t * item = mrss->item; item != NULL; item = item->next ) {
			rss_item x(ch);
			if (item->title) {
				if (item->title_type && (strcmp(item->title_type,"xhtml")==0 || strcmp(item->title_type,"html")==0)) {
					std::string xhtmltitle = utils::convert_text(item->title, "utf-8", encoding);
					x.set_title(render_xhtml_title(xhtmltitle, feed.link()));
				} else {
					std::string title = utils::convert_text(item->title, "utf-8", encoding);
					replace_newline_characters(title);
					x.set_title(title);
				}
			}
			if (item->link) {
				x.set_link(utils::absolute_url(my_uri, item->link));
			}
			if (!item->author || strcmp(item->author,"")==0) {
				if (mrss->managingeditor)
					x.set_author(utils::convert_text(mrss->managingeditor, "utf-8", encoding));
				else {
					mrss_tag_t * creator;
					if (mrss_search_tag(item, "creator", "http://purl.org/dc/elements/1.1/", &creator) == MRSS_OK && creator) {
						if (creator->value) {
							x.set_author(utils::convert_text(creator->value, "utf-8", encoding));
						}
					}
				}
			} else {
				x.set_author(utils::convert_text(item->author, "utf-8", encoding));
			}

			x.set_feedurl(feed.rssurl());

			mrss_tag_t * content;

			/*
			 * There are so many different ways in use to transport the "content" or "description".
			 * Why try a number of them to find the content, if possible:
			 * 	- "content:encoded"
			 * 	- Atom's "content"
			 * 	- Apple's "itunes:summary" that can be found in iTunes-compatible podcasts
			 * 	- last but not least, we try the standard description.
			 */
			if (mrss_search_tag(item, "encoded", "http://purl.org/rss/1.0/modules/content/", &content) == MRSS_OK && content) {
				/* RSS 2.0 content:encoded */
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: found content:encoded: %s\n", content->value);
				if (content->value) {
					std::string desc = utils::convert_text(content->value, "utf-8", encoding);
					GetLogger().log(LOG_DEBUG, "rss_parser::parse: converted description `%s' to `%s'", content->value, desc.c_str());
					x.set_description(desc);
				}
			} else {
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: found no content:encoded");
			}

			if ((mrss->version == MRSS_VERSION_ATOM_0_3 || mrss->version == MRSS_VERSION_ATOM_1_0)) {
				int rc;
				if (((rc = mrss_search_tag(item, "content", "http://www.w3.org/2005/Atom", &content)) == MRSS_OK && content) ||
					((rc = mrss_search_tag(item, "content", "http://purl.org/atom/ns#", &content)) == MRSS_OK && content)) {
					GetLogger().log(LOG_DEBUG, "rss_parser::parse: found atom content: %s\n", content ? content->value : "(content = null)");
					if (content && content->value) {
						x.set_description(utils::convert_text(content->value, "utf-8", encoding));
					}
				} else {
					GetLogger().log(LOG_DEBUG, "rss_parser::parse: mrss_search_tag(content) failed with rc = %d content = %p", rc, content);
				}
			} else {
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: not an atom feed");
			}

			/* last resort: search for itunes:summary tag (may be a podcast) */
			if (x.description().length() == 0 && mrss_search_tag(item, "summary", "http://www.itunes.com/dtds/podcast-1.0.dtd", &content) == MRSS_OK && content) {
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: found itunes:summary: %s\n", content->value);
				if (content->value) {
					/*
					 * We put the <ituneshack> tags around the tags so that the HTML renderer
					 * knows that it must not ignore the newlines. It is a really braindead
					 * use of XML to depend on the exact interpretation of whitespaces.
					 */
					std::string desc = "<ituneshack>";
					desc.append(utils::convert_text(content->value, "utf-8", encoding));
					desc.append("</ituneshack>");
					x.set_description(desc);
				}
				
			} else {
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: no luck with itunes:summary");
			}

			if (x.description().length() == 0) {
				if (item->description)
					x.set_description(utils::convert_text(item->description, "utf-8", encoding));
			} else {
				if (cfgcont->get_configvalue_as_bool("always-display-description") && item->description)
					x.set_description(x.description() + "<hr>" + utils::convert_text(item->description, "utf-8", encoding));
			}

			if (item->pubDate) 
				x.set_pubDate(parse_date(item->pubDate));
			else
				x.set_pubDate(::time(NULL));
				
			/*
			 * We try to find a GUID (some unique identifier) for an item. If the regular
			 * GUID is not available (oh, well, there are a few broken feeds around, after
			 * all), we try out the link and the title, instead. This is suboptimal, of course,
			 * because it makes it impossible to recognize duplicates when the title or the
			 * link changes.
			 */
			if (item->guid)
				x.set_guid(item->guid);
			else if (item->link)
				x.set_guid(item->link); // XXX hash something to get a better alternative GUID
			else if (item->title)
				x.set_guid(item->title);
			// ...else?! that's too bad.

			if (item->enclosure_url) {
				x.set_enclosure_url(item->enclosure_url);
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: found enclosure_url: %s", item->enclosure_url);
			}
			if (item->enclosure_type) {
				x.set_enclosure_type(item->enclosure_type);
				GetLogger().log(LOG_DEBUG, "rss_parser::parse: found enclosure_type: %s", item->enclosure_type);
			}

			// x.set_feedptr(&feed);

			GetLogger().log(LOG_DEBUG, "rss_parser::parse: item title = `%s' link = `%s' pubDate = `%s' (%d) description = `%s'", 
				x.title().c_str(), x.link().c_str(), x.pubDate().c_str(), x.pubDate_timestamp(), x.description().c_str());

			// only add item to feed if it isn't on the ignore list or if there is no ignore list
			if (!ign || !ign->matches(&x)) {
				feed.items().push_back(x);
				GetLogger().log(LOG_INFO, "rss_parser::parse: added article title = `%s' link = `%s' ign = %p", x.title().c_str(), x.link().c_str(), ign);
			} else {
				GetLogger().log(LOG_INFO, "rss_parser::parse: ignored article title = `%s' link = `%s'", x.title().c_str(), x.link().c_str());
			}
		}

		feed.remove_old_deleted_items();

		mrss_free(mrss);

	}

	feed.set_empty(false);

	return feed;
}

bool rss_parser::check_and_update_lastmodified() {
	if (my_uri.substr(0,5) != "http:" && my_uri.substr(0,6) != "https:")
		return true;

	if (ign && ign->matches_lastmodified(my_uri)) {
		GetLogger().log(LOG_DEBUG, "rss_parser::check_and_update_lastmodified: found %s on list of URLs that are always downloaded", my_uri.c_str());
		return true;
	}

	time_t oldlm = ch->get_lastmodified(my_uri);
	time_t newlm = 0;
	mrss_error_t err;

	mrss_options_t * options = create_mrss_options();
	err = mrss_get_last_modified_with_options(const_cast<char *>(my_uri.c_str()), &newlm, options);
	mrss_options_free(options);

	GetLogger().log(LOG_DEBUG, "rss_parser::check_and_update_lastmodified: err = %u oldlm = %d newlm = %d", err, oldlm, newlm);

	if (err != MRSS_OK) {
		GetLogger().log(LOG_DEBUG, "rss_parser::check_and_update_lastmodified: no, don't download, due to error");
		return false;
	}

	if (newlm == 0) {
		GetLogger().log(LOG_DEBUG, "rss_parser::check_and_update_lastmodified: yes, download (no Last-Modified header)");
		return true;
	}

	if (newlm > oldlm) {
		ch->set_lastmodified(my_uri, newlm);
		GetLogger().log(LOG_DEBUG, "rss_parser::check_and_update_lastmodified: yes, download");
		return true;
	}

	GetLogger().log(LOG_DEBUG, "rss_parser::check_and_update_lastmodified: no, don't download");
	return false;
}


// rss_item setters

void rss_item::set_title(const std::string& t) { 
	title_ = t; 
}


void rss_item::set_link(const std::string& l) { 
	link_ = l; 
}

void rss_item::set_author(const std::string& a) { 
	author_ = a; 
}

void rss_item::set_description(const std::string& d) { 
	description_ = d; 
}

void rss_item::set_pubDate(time_t t) { 
	pubDate_ = t; 
}

void rss_item::set_guid(const std::string& g) { 
	guid_ = g; 
}

void rss_item::set_unread_nowrite(bool u) {
	unread_ = u;
}

void rss_item::set_unread_nowrite_notify(bool u) {
	unread_ = u;
	if (feedptr)
		feedptr->get_item_by_guid(guid_).set_unread_nowrite(unread_); // notify parent feed
}

void rss_item::set_unread(bool u) { 
	if (unread_ != u) {
		bool old_u = unread_;
		unread_ = u;
		if (feedptr)
			feedptr->get_item_by_guid(guid_).set_unread_nowrite(unread_); // notify parent feed
		try {
			if (ch) ch->update_rssitem_unread_and_enqueued(*this, feedurl_); 
		} catch (const dbexception& e) {
			// if the update failed, restore the old unread flag and rethrow the exception
			unread_ = old_u; 
			throw e;
		}
	}
}

std::string rss_item::pubDate() const {
	char text[1024];
	strftime(text,sizeof(text),"%a, %d %b %Y %T", gmtime(&pubDate_)); 
	return std::string(text);
}

unsigned int rss_feed::unread_item_count() const {
	unsigned int count = 0;
	for (std::vector<rss_item>::const_iterator it=items_.begin();it!=items_.end();++it) {
		if (it->unread())
			++count;
	}
	return count;
}

time_t rss_parser::parse_date(const std::string& datestr) {
	std::istringstream is(datestr);
	std::string monthstr, time, tmp;
	struct tm stm;
	
	memset(&stm,0,sizeof(stm));
	
	is >> tmp;
	if (tmp[tmp.length()-1] == ',')
		is >> tmp;
	
	std::istringstream dayis(tmp);
	dayis >> stm.tm_mday;
	
	is >> monthstr;
	
	stm.tm_mon = monthname_to_number(monthstr);
	
	int year;
	is >> year;
	if (year < 100)
		year += 2000;
	stm.tm_year = year - 1900;
	
	is >> time;

    stm.tm_hour = stm.tm_min = stm.tm_sec = 0;
	
	std::vector<std::string> tkns = utils::tokenize(time,":");
	if (tkns.size() > 0) {
        std::istringstream hs(tkns[0]);
        hs >> stm.tm_hour;
        if (tkns.size() > 1) {
            std::istringstream ms(tkns[1]);
            ms >> stm.tm_min;
            if (tkns.size() > 2) {
                std::istringstream ss(tkns[2]);
                ss >> stm.tm_sec;
            }
        }
    }
	
	time_t value = mktime(&stm);
	return value;
}

bool rss_feed::matches_tag(const std::string& tag) {
	for (std::vector<std::string>::iterator it=tags_.begin();it!=tags_.end();++it) {
		if (tag == *it)
			return true;
	}
	return false;
}

std::string rss_feed::get_tags() {
	std::string tags;
	for (std::vector<std::string>::iterator it=tags_.begin();it!=tags_.end();++it) {
		if (it->substr(0,1) == "~") {
			tags.append(*it);
			tags.append(" ");
		}
	}
	return tags;
}

void rss_feed::set_tags(const std::vector<std::string>& tags) {
	if (tags_.size() > 0)
		tags_.erase(tags_.begin(), tags_.end());
	for (std::vector<std::string>::const_iterator it=tags.begin();it!=tags.end();++it) {
		tags_.push_back(*it);
	}
}

void rss_item::set_enclosure_url(const std::string& url) {
	enclosure_url_ = url;
}

void rss_item::set_enclosure_type(const std::string& type) {
	enclosure_type_ = type;
}

std::string rss_item::title() const {
	GetLogger().log(LOG_DEBUG,"rss_item::title: title before conversion: %s", title_.c_str());
	std::string retval;
	if (title_.length()>0)
		retval = utils::convert_text(title_, nl_langinfo(CODESET), "utf-8");
	GetLogger().log(LOG_DEBUG,"rss_item::title: title after conversion: %s", retval.c_str());
	return retval;
}

std::string rss_item::author() const {
	return utils::convert_text(author_, nl_langinfo(CODESET), "utf-8");
}

std::string rss_item::description() const {
	return utils::convert_text(description_, nl_langinfo(CODESET), "utf-8");
}

std::string rss_feed::title() const {
	bool found_title = false;
	std::string alt_title;
	for (std::vector<std::string>::const_iterator it=tags_.begin();it!=tags_.end();it++) {
		if (it->substr(0,1) == "~") {
			found_title = true;
			alt_title = it->substr(1, it->length()-1);
			break;
		}
	}
	return found_title ? alt_title : utils::convert_text(title_, nl_langinfo(CODESET), "utf-8");
}

std::string rss_feed::description() const {
	return utils::convert_text(description_, nl_langinfo(CODESET), "utf-8");
}

rss_item& rss_feed::get_item_by_guid(const std::string& guid) {
	for (std::vector<rss_item>::iterator it=items_.begin();it!=items_.end();++it) {
		if (it->guid() == guid) {
			return *it;
		}
	}
	GetLogger().log(LOG_DEBUG, "rss_feed::get_item_by_guid: hit dummy item!");
	// abort();
	static rss_item dummy_item(0); // should never happen!
	return dummy_item;
}

bool rss_item::has_attribute(const std::string& attribname) {
	// GetLogger().log(LOG_DEBUG, "rss_item::has_attribute(%s) called", attribname.c_str());
	if (attribname == "title" || 
		attribname == "link" || 
		attribname == "author" || 
		attribname == "content" || 
		attribname == "date"  ||
		attribname == "guid" ||
		attribname == "unread" ||
		attribname == "enclosure_url" ||
		attribname == "enclosure_type" ||
		attribname == "flags")
			return true;

	// if we have a feed, then forward the request
	if (feedptr)
		return feedptr->rss_feed::has_attribute(attribname);

	return false;
}

std::string rss_item::get_attribute(const std::string& attribname) {
	if (attribname == "title")
		return title();
	else if (attribname == "link")
		return link();
	else if (attribname == "author")
		return author();
	else if (attribname == "content")
		return description();
	else if (attribname == "date")
		return pubDate();
	else if (attribname == "guid")
		return guid();
	else if (attribname == "unread")
		return unread_ ? "yes" : "no";
	else if (attribname == "enclosure_url")
		return enclosure_url();
	else if (attribname == "enclosure_type")
		return enclosure_type();
	else if (attribname == "flags")
		return flags();

	// if we have a feed, then forward the request
	if (feedptr)
		return feedptr->rss_feed::get_attribute(attribname);

	return "";
}

void rss_item::update_flags() {
	if (ch) {
		ch->update_rssitem_flags(*this);
	}
}

void rss_item::sort_flags() {
	std::sort(flags_.begin(), flags_.end());

	for (std::string::iterator it=flags_.begin();flags_.size() > 0 && it!=flags_.end();++it) {
		if (!isalpha(*it)) {
			flags_.erase(it);
			it = flags_.begin();
		}
	}

	for (unsigned int i=0;i<flags_.size();++i) {
		if (i < (flags_.size()-1)) {
			if (flags_[i] == flags_[i+1]) {
				flags_.erase(i+1,i+1);
				--i;
			}
		}
	}
}

bool rss_feed::has_attribute(const std::string& attribname) {
	if (attribname == "feedtitle" ||
		attribname == "description" ||
		attribname == "feedlink" ||
		attribname == "feeddate" ||
		attribname == "rssurl" ||
		attribname == "unread_count" ||
		attribname == "total_count" ||
		attribname == "tags")
			return true;
	return false;
}

std::string rss_feed::get_attribute(const std::string& attribname) {
	if (attribname == "feedtitle")
		return title();
	else if (attribname == "description")
		return description();
	else if (attribname == "feedlink")
		return title();
	else if (attribname == "feeddate")
		return pubDate();
	else if (attribname == "rssurl")
		return rssurl();
	else if (attribname == "unread_count") {
		return utils::to_s(unread_item_count());
	} else if (attribname == "total_count") {
		return utils::to_s(items_.size());
	} else if (attribname == "tags") {
		return get_tags();
	}
	return "";
}

action_handler_status rss_ignores::handle_action(const std::string& action, const std::vector<std::string>& params) {
	if (action == "ignore-article") {
		if (params.size() >= 2) {
			std::string ignore_rssurl = params[0];
			std::string ignore_expr = params[1];
			matcher m;
			if (m.parse(ignore_expr)) {
				ignores.push_back(feedurl_expr_pair(ignore_rssurl, new matcher(ignore_expr)));
				return AHS_OK;
			} else {
				return AHS_INVALID_PARAMS;
			}
		} else {
			return AHS_TOO_FEW_PARAMS;
		}
	} else if (action == "always-download") {
		for (std::vector<std::string>::const_iterator it=params.begin();it!=params.end();++it) {
			ignores_lastmodified.push_back(*it);
		}
		return AHS_OK;
	} else if (action == "reset-unread-on-update") {
		for (std::vector<std::string>::const_iterator it=params.begin();it!=params.end();++it) {
			resetflag.push_back(*it);
		}
		return AHS_OK;
	}
	return AHS_INVALID_COMMAND;
}

rss_ignores::~rss_ignores() {
	for (std::vector<feedurl_expr_pair>::iterator it=ignores.begin();it!=ignores.end();++it) {
		delete it->second;
	}
}

bool rss_ignores::matches(rss_item* item) {
	for (std::vector<feedurl_expr_pair>::iterator it=ignores.begin();it!=ignores.end();++it) {
		GetLogger().log(LOG_DEBUG, "rss_ignores::matches: it->first = `%s' item->feedurl = `%s'", it->first.c_str(), item->feedurl().c_str());
		if (it->first == "*" || item->feedurl() == it->first) {
			if (it->second->matches(item)) {
				GetLogger().log(LOG_DEBUG, "rss_ignores::matches: found match");
				return true;
			}
		}
	}
	return false;
}

bool rss_ignores::matches_lastmodified(const std::string& url) {
	for (std::vector<std::string>::iterator it=ignores_lastmodified.begin();it!=ignores_lastmodified.end();++it) {
		if (url == *it)
			return true;
	}
	return false;
}

bool rss_ignores::matches_resetunread(const std::string& url) {
	for (std::vector<std::string>::iterator it=resetflag.begin();it!=resetflag.end();++it) {
		if (url == *it)
			return true;
	}
	return false;
}

void rss_feed::update_items(std::vector<rss_feed>& feeds) {
	if (query.length() == 0)
		return;

	GetLogger().log(LOG_DEBUG, "rss_feed::update_items: query = `%s'", query.c_str());


	struct timeval tv1, tv2, tvx;
	gettimeofday(&tv1, NULL);

	matcher m(query);

	if (items_.size() > 0) {
		items_.erase(items_.begin(), items_.end());
	}

	for (std::vector<rss_feed>::iterator it=feeds.begin();it!=feeds.end();++it) {
		if (it->rssurl().substr(0,6) != "query:") { // don't fetch items from other query feeds!
			for (std::vector<rss_item>::iterator jt=it->items().begin();jt!=it->items().end();++jt) {
				if (m.matches(&(*jt))) {
					GetLogger().log(LOG_DEBUG, "rss_feed::update_items: matcher matches!");
					jt->set_feedptr(&(*it));
					items_.push_back(*jt);
				}
			}
		}
	}

	gettimeofday(&tvx, NULL);

	std::sort(items_.begin(), items_.end());

	gettimeofday(&tv2, NULL);
	unsigned long diff = (((tv2.tv_sec - tv1.tv_sec) * 1000000) + tv2.tv_usec) - tv1.tv_usec;
	unsigned long diffx = (((tv2.tv_sec - tvx.tv_sec) * 1000000) + tv2.tv_usec) - tvx.tv_usec;
	GetLogger().log(LOG_DEBUG, "rss_feed::update_items matching took %lu.%06lu s", diff / 1000000, diff % 1000000);
	GetLogger().log(LOG_DEBUG, "rss_feed::update_items sorting took %lu.%06lu s", diffx / 1000000, diffx % 1000000);
}

void rss_feed::set_rssurl(const std::string& u) {
	rssurl_ = u;
	if (rssurl_.substr(0,6) == "query:") {
		std::vector<std::string> tokens = utils::tokenize_quoted(u, ":");
		GetLogger().log(LOG_DEBUG, "rss_feed::set_rssurl: query name = `%s' expr = `%s'", tokens[1].c_str(), tokens[2].c_str());
		set_title(tokens[1]);
		set_query(tokens[2]);
	}
}

struct sort_item_by_title : public std::binary_function<const rss_item&, const rss_item&, bool> {
	bool operator()(const rss_item& a, const rss_item& b) {
		return strcasecmp(a.title().c_str(), b.title().c_str()) < 0;
	}
};

struct sort_item_by_flags : public std::binary_function<const rss_item&, const rss_item&, bool> {
	bool operator()(const rss_item& a, const rss_item& b) {
		return strcmp(a.flags().c_str(), b.flags().c_str()) < 0;
	}
};

struct sort_item_by_author : public std::binary_function<const rss_item&, const rss_item&, bool> {
	bool operator()(const rss_item& a, const rss_item& b) {
		return strcmp(a.author().c_str(), b.author().c_str()) < 0;
	}
};

struct sort_item_by_link : public std::binary_function<const rss_item&, const rss_item&, bool> {
	bool operator()(const rss_item& a, const rss_item& b) {
		return strcmp(a.link().c_str(), b.link().c_str()) < 0;
	}
};

struct sort_item_by_guid : public std::binary_function<const rss_item&, const rss_item&, bool> {
	bool operator()(const rss_item& a, const rss_item& b) {
		return strcmp(a.guid().c_str(), b.guid().c_str()) < 0;
	}
};

void rss_feed::sort(const std::string& method) {
	std::vector<std::string> methods = utils::tokenize(method,"-");
	bool reverse = false;

	if (methods.size() > 0 && methods[0] == "date") { // date is descending by default
		if (methods.size() > 1 && methods[1] == "asc") {
			reverse = true;
		}
	} else { // all other sort methods are ascending by default
		if (methods.size() > 1 && methods[1] == "desc") {
			reverse = true;
		}
	}

	if (methods.size() > 0 && methods[0] != "date") {
		if (methods[0] == "title") {
			std::stable_sort(items_.begin(), items_.end(), sort_item_by_title());
		} else if (methods[0] == "flags") {
			std::stable_sort(items_.begin(), items_.end(), sort_item_by_flags());
		} else if (methods[0] == "author") {
			std::stable_sort(items_.begin(), items_.end(), sort_item_by_author());
		} else if (methods[0] == "link") {
			std::stable_sort(items_.begin(), items_.end(), sort_item_by_link());
		} else if (methods[0] == "guid") {
			std::stable_sort(items_.begin(), items_.end(), sort_item_by_guid());
		} // add new sorting methods here
	}

	if (reverse) {
		std::reverse(items_.begin(), items_.end());
	}
}

void rss_feed::remove_old_deleted_items() {
	std::vector<std::string> guids;
	for (std::vector<rss_item>::iterator it=items_.begin();it!=items_.end();++it) {
		guids.push_back(it->guid());
	}
	ch->remove_old_deleted_items(rssurl_, guids);
}

void rss_feed::purge_deleted_items() {
	scope_measure m1("rss_feed::purge_deleted_items");
	std::vector<rss_item>::iterator it=items_.begin();
	while (it!=items_.end()) {
		if (it->deleted()) {
			items_.erase(it);
			it = items_.begin(); // items_ modified -> iterator invalidated
		} else {
			++it;
		}
	}
}

void rss_item::set_feedptr(rss_feed * ptr) {
	feedptr = ptr;
}

void rss_parser::replace_newline_characters(std::string& str) {
	str = utils::replace_all(str, "\r", " ");
	str = utils::replace_all(str, "\n", " ");
}

mrss_options_t * rss_parser::create_mrss_options() {
	char * proxy = NULL;
	char * proxy_auth = NULL;

	if (cfgcont->get_configvalue_as_bool("use-proxy") == true) {
		proxy = const_cast<char *>(cfgcont->get_configvalue("proxy").c_str());
		proxy_auth = const_cast<char *>(cfgcont->get_configvalue("proxy-auth").c_str());
	}

	return mrss_options_new(30, proxy, proxy_auth, NULL, NULL, NULL, 0, NULL, utils::get_useragent(cfgcont).c_str());
}

std::string rss_parser::render_xhtml_title(const std::string& title, const std::string& link) {
	htmlrenderer rnd(1 << 16); // a huge number
	std::vector<std::string> lines;
	std::vector<linkpair> links; // not needed
	rnd.render(title, lines, links, link);
	if (lines.size() > 0)
		return lines[0];
	return "";
}

unsigned int rss_parser::monthname_to_number(const std::string& monthstr) {
	static const char * monthtable[] = { "Jan", "Feb", "Mar", "Apr","May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL };
	for (unsigned int i=0;monthtable[i]!=NULL;i++) {
		if (monthstr == monthtable[i])
			return i;
	}
	return 0;
}

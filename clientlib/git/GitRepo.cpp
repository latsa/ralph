/* Copyright 2016 Jan Dalheimer <jan@dalheimer.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GitRepo.h"

#include <QUrl>

#include <git2.h>

namespace Ralph {
namespace ClientLib {
namespace Git {

void initGit()
{
	static bool haveInitialized = false;
	if (!haveInitialized) {
		git_libgit2_init();
	}
}

std::function<GitCredentialResponse(const GitCredentialQuery &)> GitRepo::m_credentialsFunc;

GitRepo::GitRepo(const QDir &dir)
	: m_dir(dir)
{
	initGit();
}

Future<GitRepo *> GitRepo::init(const QDir &dir)
{
	return async([dir](Notifier)
	{
		initGit();
		if (!dir.mkpath(dir.absolutePath())) {
			throw Exception("Unable to create directory to init");
		}

		std::unique_ptr<GitRepo> repo = std::make_unique<GitRepo>(dir);

		git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
		opts.flags |= GIT_REPOSITORY_INIT_MKPATH;
		repo->m_repo = GitResource<git_repository>::create(&git_repository_init_ext, &git_repository_free,
														   dir.absolutePath().toLocal8Bit(), &opts);

		return repo.release();
	});
}

Future<GitRepo *> GitRepo::open(const QDir &dir)
{
	return async([dir](Notifier)
	{
		initGit();

		std::unique_ptr<GitRepo> repo = std::make_unique<GitRepo>(dir);

		repo->m_repo = GitResource<git_repository>::create(&git_repository_open_ext, &git_repository_free,
														   dir.absolutePath().toLocal8Bit(), GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);

		return repo.release();
	});
}

struct GitPayload
{
	Notifier notifier;
	QString identifier;
	QVariant payload;
	enum { Initial, Fetching, CheckingOut } state = Initial;
};

static void gitCheckoutNotifier(const char *, const size_t current, const size_t total, void *payload)
{
	GitPayload *pl = static_cast<GitPayload *>(payload);
	if (pl->state != GitPayload::CheckingOut) {
		pl->notifier.status("Checking out %1..." % pl->identifier);
		pl->state = GitPayload::CheckingOut;
	}
	pl->notifier.progress(current, total);
}
static int gitFetchNotifier(const git_transfer_progress *stats, void *payload)
{
	GitPayload *pl = static_cast<GitPayload *>(payload);
	if (pl->state != GitPayload::Fetching) {
		pl->notifier.status("Fetching...");
		pl->state = GitPayload::Fetching;
	}
	pl->notifier.progress(stats->received_objects, stats->total_objects);

	return 0;
}

Future<GitRepo *> GitRepo::clone(const QDir &dir, const QUrl &url)
{
	return async([dir, url](Notifier notifier)
	{
		initGit();

		notifier.status("Cloning %1..." % url.toString());

		std::unique_ptr<GitRepo> repo = std::make_unique<GitRepo>(dir);

		GitPayload payload{notifier};

		git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
		opts.checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_USE_THEIRS;
		opts.checkout_opts.progress_cb = &gitCheckoutNotifier;
		opts.checkout_opts.progress_payload = &payload;
		opts.fetch_opts.callbacks.transfer_progress = &gitFetchNotifier;
		opts.fetch_opts.callbacks.payload = &payload;
		opts.fetch_opts.callbacks.credentials = &credentialsCallback;

		repo->m_repo = GitResource<git_repository>::create(&git_clone, &git_repository_free,
														   url.toString().toLocal8Bit(), dir.absolutePath().toLocal8Bit(), &opts);

		return repo.release();
	});
}

Future<void> GitRepo::fetch() const
{
	return async([this](Notifier notifier)
	{
		auto remote = GitResource<git_remote>::create(&git_remote_lookup, &git_remote_free, m_repo, "origin");

		GitPayload payload{notifier};

		git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
		opts.callbacks.transfer_progress = &gitFetchNotifier;
		opts.callbacks.payload = &payload;
		opts.callbacks.credentials = &credentialsCallback;

		GitException::checkAndThrow(git_remote_fetch(remote, nullptr, &opts, nullptr));
	});
}
Future<void> GitRepo::checkout(const QString &id) const
{
	return async([this, id](Notifier notifier)
	{
		auto treeish = GitResource<git_object>::create(&git_revparse_single, &git_object_free, m_repo, id.toLocal8Bit().constData());

		git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
		opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_USE_THEIRS;
		opts.progress_cb = &gitCheckoutNotifier;
		GitPayload payload{notifier, id};
		opts.progress_payload = &payload;

		GitException::checkAndThrow(git_checkout_tree(m_repo, treeish, &opts));
	});
}
Future<void> GitRepo::pull(const QString &id) const
{
	return async([this, id](Notifier notifier)
	{
		notifier.await(fetch());
		notifier.await(checkout(id));
	});
}

static int gitSubmoduleUpdate(git_submodule *sm, const char *, void *payload)
{
	GitPayload *pl = static_cast<GitPayload *>(payload);

	git_submodule_update_options opts = GIT_SUBMODULE_UPDATE_OPTIONS_INIT;
	opts.checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_USE_THEIRS;
	opts.checkout_opts.progress_cb = &gitCheckoutNotifier;
	opts.checkout_opts.progress_payload = payload;
	opts.fetch_opts.callbacks.transfer_progress = &gitFetchNotifier;
	opts.fetch_opts.callbacks.payload = payload;
	opts.fetch_opts.callbacks.credentials = &GitRepo::credentialsCallback;

	return git_submodule_update(sm, pl->payload.toBool(), &opts);
}
Future<void> GitRepo::submodulesUpdate(const bool init) const
{
	return async([this, init](Notifier notifier)
	{
		GitPayload payload{notifier, QString(), init};
		GitException::checkAndThrow(git_submodule_foreach(m_repo, &gitSubmoduleUpdate, &payload));
	});
}

GitCredentialResponse::GitCredentialResponse(git_cred *cred)
	: m_cred(cred) {}
GitCredentialResponse GitCredentialResponse::createForUsername(const QString &username)
{
	git_cred *cred;
	if (git_cred_username_new(&cred, username.toLocal8Bit().constData()) < 0) {
		return createError();
	} else {
		return GitCredentialResponse(cred);
	}
}
GitCredentialResponse GitCredentialResponse::createForUsernamePassword(const QString &username, const QString &password)
{
	git_cred *cred;
	if (git_cred_userpass_plaintext_new(&cred,
										username.toLocal8Bit().constData(),
										password.toLocal8Bit().constData()) < 0) {
		return createError();
	} else {
		return GitCredentialResponse(cred);
	}
}
GitCredentialResponse GitCredentialResponse::createForSSHKey(const QString &username, const QString &pubkeyPath, const QString &privkeyPath, const QString &passthrase)
{
	git_cred *cred;
	if (git_cred_ssh_key_new(&cred,
							 username.toLocal8Bit().constData(),
							 pubkeyPath.toLocal8Bit().constData(),
							 privkeyPath.toLocal8Bit().constData(),
							 passthrase.toLocal8Bit().constData()) < 0) {
		return createError();
	} else {
		return GitCredentialResponse(cred);
	}
}
GitCredentialResponse GitCredentialResponse::createForDefault()
{
	git_cred *cred;
	if (git_cred_default_new(&cred) < 0) {
		return createError();
	} else {
		return GitCredentialResponse(cred);
	}
}
GitCredentialResponse GitCredentialResponse::createInvalid()
{
	return GitCredentialResponse(nullptr);
}
GitCredentialResponse GitCredentialResponse::createError()
{
	return GitCredentialResponse();
}

int GitRepo::credentialsCallback(git_cred **out, const char *url, const char *usernameFromUrl, const unsigned int allowedTypes, void *)
{
	GitCredentialQuery::Types types;
	if (allowedTypes & GIT_CREDTYPE_DEFAULT) {
		types |= GitCredentialQuery::Default;
	}
	if (allowedTypes & GIT_CREDTYPE_USERNAME) {
		types |= GitCredentialQuery::Username;
	}
	if (allowedTypes & GIT_CREDTYPE_USERPASS_PLAINTEXT) {
		types |= GitCredentialQuery::UsernamePassword;
	}
	if (allowedTypes & GIT_CREDTYPE_SSH_CUSTOM) {
		types |= GitCredentialQuery::SSHCustom;
	}
	if (allowedTypes & GIT_CREDTYPE_SSH_INTERACTIVE) {
		types |= GitCredentialQuery::SSHInteractive;
	}
	if (allowedTypes & GIT_CREDTYPE_SSH_KEY) {
		types |= GitCredentialQuery::SSHKey;
	}
	const GitCredentialQuery query = GitCredentialQuery(types, QString::fromLocal8Bit(url), QString::fromLocal8Bit(usernameFromUrl));
	const GitCredentialResponse response = m_credentialsFunc(query);
	if (!response.result()) {
		return response.isError() ? GIT_EUSER : 1;
	} else {
		*out = response.result();
		return 0;
	}
}

GitCredentialQuery::GitCredentialQuery(const GitCredentialQuery::Types types, const QUrl &url, const QString &usernameDefault)
	: m_types(types), m_url(url), m_usernameFromUrl(usernameDefault) {}

}
}
}

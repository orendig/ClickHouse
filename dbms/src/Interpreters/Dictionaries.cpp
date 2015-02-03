#include <DB/Interpreters/Dictionaries.h>
#include <DB/Dictionaries/DictionaryFactory.h>
#include <DB/Dictionaries/DictionaryStructure.h>
#include <DB/Dictionaries/IDictionarySource.h>
#include <DB/Dictionaries/config_ptr_t.h>

namespace DB
{

void Dictionaries::reloadExternals()
{
	const auto config_path = Poco::Util::Application::instance().config().getString("dictionaries_config");
	if (config_path.empty())
		return;

	const auto last_modified = Poco::File{config_path}.getLastModified();
	if (last_modified > dictionaries_last_modified)
	{
		/// definitions of dictionaries may have changed, recreate all of them
		dictionaries_last_modified = last_modified;

		const config_ptr_t<Poco::Util::XMLConfiguration> config{new Poco::Util::XMLConfiguration{config_path}};

		/// get all dictionaries' definitions
		Poco::Util::AbstractConfiguration::Keys keys;
		config->keys(keys);

		/// for each dictionary defined in xml config
		for (const auto & key : keys)
		{
			try
			{
				if (0 != strncmp(key.data(), "dictionary", strlen("dictionary")))
				{
					LOG_WARNING(log, "unknown node in dictionaries file: '" + key + "', 'dictionary'");
					continue;
				}

				const auto & prefix = key + '.';

				const auto & name = config->getString(prefix + "name");
				if (name.empty())
				{
					LOG_WARNING(log, "dictionary name cannot be empty");
					continue;
				}

				auto dict_ptr = DictionaryFactory::instance().create(name, *config, prefix, context);
				if (!dict_ptr->isCached())
				{
					const auto & lifetime = dict_ptr->getLifetime();
					std::uniform_int_distribution<std::uint64_t> distribution{lifetime.min_sec, lifetime.max_sec};
					update_times[name] = std::chrono::system_clock::now() +
						std::chrono::seconds{distribution(rnd_engine)};
				}

				auto it = external_dictionaries.find(name);
				/// add new dictionary or update an existing version
				if (it == std::end(external_dictionaries))
				{
					const std::lock_guard<std::mutex> lock{external_dictionaries_mutex};
					external_dictionaries.emplace(name, std::make_shared<MultiVersion<IDictionary>>(dict_ptr.release()));
				}
				else
					it->second->set(dict_ptr.release());
			}
			catch (...)
			{
				handleException();
			}
		}
	}
	else
	{
		/// periodic update
		for (auto & dictionary : external_dictionaries)
		{
			try
			{
				auto current = dictionary.second->get();
				/// update only non-cached dictionaries
				if (!current->isCached())
				{
					auto & update_time = update_times[current->getName()];

					/// check that timeout has passed
					if (std::chrono::system_clock::now() < update_time)
						continue;

					/// check source modified
					if (current->getSource()->isModified())
					{
						/// create new version of dictionary
						auto new_version = current->clone();
						dictionary.second->set(new_version.release());
					}

					/// calculate next update time
					const auto & lifetime = current->getLifetime();
					std::uniform_int_distribution<std::uint64_t> distribution{lifetime.min_sec, lifetime.max_sec};
					update_time = std::chrono::system_clock::now() + std::chrono::seconds{distribution(rnd_engine)};
				}
			}
			catch (...)
			{
				handleException();
			}
		}
	}
}

}

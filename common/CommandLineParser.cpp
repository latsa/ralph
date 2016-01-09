#include "CommandLineParser.h"

#include <QStringList>
#include <QCoreApplication>
#include <QRegularExpression>

#include <iostream>

#include "TermUtil.h"

namespace Ralph {
namespace Common {
namespace CommandLine {

class CommandLineException : public Exception
{
public:
	explicit CommandLineException(const QString &message, const QVector<QString> &commandChain)
		: Exception(message), m_commandChain(commandChain) {}
	CommandLineException(const CommandLineException &) = default;
	virtual ~CommandLineException();

	QVector<QString> commandChain() const { return m_commandChain; }

private:
	QVector<QString> m_commandChain;
};

#define DEC_EXCEPTION(name) \
	QT_WARNING_PUSH \
	QT_WARNING_DISABLE_GCC("-Wweak-vtables") \
	class name##Exception : public CommandLineException { public: using CommandLineException::CommandLineException; } \
	QT_WARNING_POP
DEC_EXCEPTION(ToManyPositionals);
DEC_EXCEPTION(UnknownOption);
DEC_EXCEPTION(UnexpectedArgument);
DEC_EXCEPTION(MissingRequiredArgument);
DEC_EXCEPTION(MalformedOption);
DEC_EXCEPTION(MissingPositionalArgument);
DEC_EXCEPTION(InvalidOptionValue);
DEC_EXCEPTION(Build);

int Parser::process(int argc, char **argv)
{
	QStringList list;
	for (int i = 0; i < argc; ++i) {
		list.append(QString::fromLocal8Bit(argv[0]));
	}
	return process(list);
}
int Parser::process(const QCoreApplication &app)
{
	setVersion(app.applicationVersion());
	setName(app.applicationName());
	return process(app.arguments());
}
int Parser::process(const QStringList &arguments)
{
	try {
		const Result result = parse(arguments);
		handle(result);
		if (result.options().isEmpty() && result.arguments().isEmpty() && result.commandChain().size() == 1) {
			printHelp();
		}
		return 0;
	} catch (BuildException &e) {
		std::cerr << e.what() << '\n'
				  << "This is a logic error in the program. Please report it to the developer.\n";
		return -1;
	} catch (CommandLineException &e) {
		std::cerr << e.what() << "\n\n";
		printHelp(e.commandChain());
	} catch (Exception &e) {
		std::cerr << e.what() << '\n';
		return -1;
	}
}

Parser &Parser::addVersionCommand()
{
	add(Command("version", "Show the version of this program").then([this]() { printVersion(); }));
	return *this;
}
Parser &Parser::addVersionOption()
{
	add(Option({"version", "v"})
		.setDescription("Show the version of this program")
		.setEarlyExit(true)
		.then([this]() { printVersion(); }));
	return *this;
}
Parser &Parser::addHelpCommand()
{
	add(Command("help", "Shows help for a given command")
		.add(PositionalArgument("subcommand", "The subcommand to show help for").setMulti(true).setOptional(true))
		.then([this](const Result &r) { printHelp(r.commandChain().mid(0, r.commandChain().size() - 1) + r.argumentMulti("subcommand")); }));
	return *this;
}
Parser &Parser::addHelpOption()
{
	add(Option({"help", "h"})
		.setDescription("Show help for the given command")
		.setEarlyExit(true)
		.then([this](const Result &r) { printHelp(r.commandChain()); }));
	return *this;
}

void Parser::printVersion()
{
	std::cout << name() << " - " << version() << '\n';
	std::exit(0);
}

QString formatPositionalArgument(const PositionalArgument &arg)
{
	QString result;
	if (arg.isOptional()) {
		result += '[';
	}
	result += '<' + arg.name();
	if (arg.isMulti()) {
		result += "...";
	}
	result += '>';
	if (arg.isOptional()) {
		result += ']';
	}
	return result;
}
QVector<QString> formatPositionalArguments(const QVector<PositionalArgument> &args)
{
	return Functional::map(args, &formatPositionalArgument);
}
static void printUsageFor(const QStringList &parents, const bool hasOptions, const QVector<QString> &positionals, const Command &command, const int maxWidth)
{
	QVector<QString> posArgs = positionals + formatPositionalArguments(command.arguments());
	if (!parents.isEmpty()) {
		std::cout << "    " << parents.join(' ');
		if (hasOptions) {
			std::cout << " [OPTIONS]";
		}

		if (!posArgs.isEmpty()) {
			std::cout << ' ' << posArgs.toList().join(' ');
		}
		std::cout << '\n';
	}

	for (const Command &sub : command.subcommands()) {
		printUsageFor(parents + QStringList(sub.name()), hasOptions || !command.options().isEmpty(), posArgs, sub, maxWidth);
	}
}
static void printSubcommandsTable(const QVector<Command> &commands, const int maxWidth)
{
	const QVector<QVector<QString>> rows = Functional::map(commands, [](const Command &command)
	{
		return QVector<QString>({command.name(), "-", command.summary()});
	});
	std::cout << "    " << Term::table(rows, {10, 1, 10}, maxWidth, 4) << '\n';
}
static void printOptionsTable(const QVector<Option> &options, const int maxWidth)
{
	const QVector<QVector<QString>> rows = Functional::map(options, [](const Option &option)
	{
		const QList<QString> syntax = Functional::map(option.names(), [option](const QString &name)
		{
			QString variant = QString('-');
			if (name.size() > 1) {
				variant += '-';
			}
			variant += name;
			if (option.hasArgument()) {
				if (option.isArgumentRequired()) {
					variant += "=<" + option.argument() + '>';
				} else  {
					variant += "[=<" + option.argument() + ">]";
				}
			}
			return variant;
		});

		QList<QString> help = QList<QString>() << option.description();
		if (option.hasArgument()) {
			if (!option.defaultValue().isNull()) {
				help.append(Term::style(Term::Bold, "Default: ") + option.defaultValue());
			}
			if (!option.allowedValues().isEmpty()) {
				help.append(Term::style(Term::Bold, "Allowed: ") + option.allowedValues().toList().join(", "));
			}
		}

		return QVector<QString>({syntax.join(", "), help.join('\n')});
	});
	std::cout << "    " << Term::table(rows, {1, 1}, maxWidth, 4) << '\n';
}
static void printArgumentsTable(const QVector<PositionalArgument> &arguments, const int maxWidth)
{
	const QVector<QVector<QString>> rows = Functional::map(arguments, [](const PositionalArgument &argument)
	{
		const QString syntax = formatPositionalArgument(argument);
		return QVector<QString>({syntax, argument.description()});
	});
	std::cout << "    " << Term::table(rows, {1, 1}, maxWidth, 4) << '\n';
}
void Parser::printHelp(const QVector<QString> &commands)
{
	const int maxWidth = Term::currentWidth() != 0 ? Term::currentWidth() : 120;

	QVector<QString> chain = commands.isEmpty() ? QVector<QString>({name()}) : commands;
	QVector<Option> options;
	QVector<PositionalArgument> positionals;
	Command command = Command(QString()).add(*this);
	for (const QString &cmd : chain) {
		command = command.subcommands().value(cmd);
		options.append(command.options());
		positionals.append(command.arguments());
	}

	using namespace Term;

	std::cout << name() << ' ' << version() << '\n'
			  << '\n'
			  << style(Bold, "Usage:") << '\n';

	printUsageFor(chain.toList(), !options.isEmpty(), {}, command, maxWidth);

	if (!command.subcommands().isEmpty()) {
		std::cout << '\n'
				  << style(Bold, "Subcommands:") << '\n';
		printSubcommandsTable(command.subcommands().values().toVector(), maxWidth);
	}
	if (!options.isEmpty()) {
		std::cout << '\n'
				  << style(Bold, "Options:") << '\n';
		printOptionsTable(options, maxWidth);
	}
	if (!positionals.isEmpty()) {
		std::cout << '\n'
				  << style(Bold, "Arguments:") << '\n';
		printArgumentsTable(positionals, maxWidth);
	}
	if (!command.description().isEmpty()) {
		std::cout << '\n'
				  << style(Bold, "Description:") << '\n';
		std::cout << "    " << wrap(command.description(), maxWidth, 4) << '\n';
	}

	std::exit(0);
}

Result Parser::parse(const QStringList &arguments) const
{
	static const QRegularExpression doubleOptionExpression("^--(?<name>[A-Za-z0-9-\\.]+)(?<valuecont>=(?<value>.*))?$");
	static const QRegularExpression singleOptionExpression("^-(?<names>[A-Za-z0-9]+)(?<valuecont>=(?<value>.*))?$");

	struct {
		QHash<QString, QString> options;
		QHash<QString, QVector<QString>> arguments;
		QVector<QString> commandChain;
	} result;

	Command currentCommand;
	QHash<QString, Option> options;
	QVector<PositionalArgument> positionals;
	bool haveStartedPositionals = false; // after the first positional we don't allow any more subcommands

	auto nextCommand = [&currentCommand, &options, &positionals, &result](const Command &command)
	{
		for (const Option &option : command.options()) {
			for (const QString &name : option.names()) {
				options.insert(name, option);
			}
		}
		positionals.append(command.arguments());
		checkPositionals(positionals);
		currentCommand = command;
		result.commandChain.append(command.name());
	};
	auto resolveAlias = [&currentCommand, &nextCommand](const QVector<QString> &path)
	{
		for (const QString &cmd : path) {
			nextCommand(currentCommand.subcommands().value(cmd));
		}
	};
	auto handleOption = [&result, &options](const QString &name, const QString &value, const bool hasValue, QStringListIterator *it)
	{
		if (!options.contains(name)) {
			throw UnknownOptionException("Unknown option: --" + name);
		}
		const Option &option = options.value(name);
		if (hasValue && !option.hasArgument()) {
			throw UnexpectedArgumentException("Didn't expect an argument in " + (it ? it->peekPrevious() : name));
		} else if (!hasValue && option.hasArgument() && option.isArgumentRequired()) {
			if (it && it->hasNext() && !it->peekNext().startsWith('-')) {
				result.options.insert(option.names().first(), it->next());
				return;
			} else {
				throw MissingRequiredArgumentException("Missing required argument to --" + name);
			}
		}
		result.options.insert(option.names().first(), value);
	};
	auto handleArguments = [&result, &haveStartedPositionals, &positionals](const QVector<QString> &args)
	{
		const int availablePositionals = Functional::collection(result.arguments.values()).mapSize().sum() + args.size();
		const bool haveMulti = !positionals.isEmpty() && positionals.last().isMulti();
		if (availablePositionals > positionals.size() && !haveMulti) {
			throw ToManyPositionalsException(QString("Expected no more than %1 positional arguments, got %2").arg(positionals.size()).arg(availablePositionals));
		}
		for (const QString &arg : args) {
			if (positionals.size() == result.arguments.size()) {
				result.arguments[positionals.last().name()].append(arg);
			} else {
				result.arguments.insert(positionals.at(result.arguments.size()).name(), {arg});
			}
		}
		haveStartedPositionals = true;
	};

	nextCommand(*this);

	QStringListIterator it(arguments);
	it.next(); // application name
	while (it.hasNext()) {
		const QString item = it.next();
		if (!haveStartedPositionals && currentCommand.subcommands().contains(item)) {
			nextCommand(currentCommand.subcommands().value(item));
		} else if (!haveStartedPositionals && currentCommand.commandAliases().contains(item)) {
			resolveAlias(currentCommand.commandAliases().value(item));
		} else if (item == "--") {
			// everything after a -- needs to be positional arguments
			QVector<QString> remaining;
			while (it.hasNext()) {
				remaining.append(it.next());
			}
			handleArguments(remaining);
		} else if (item.startsWith("--")) {
			const QRegularExpressionMatch match = doubleOptionExpression.match(item);
			if (!match.hasMatch()) {
				throw MalformedOptionException("Malformed option: " + item);
			}
			const QString name = match.captured("name");
			const QString value = match.captured("value");
			const bool hasValue = !match.captured("valuecont").isEmpty();
			handleOption(name, value, hasValue, &it);
		} else if (item.startsWith('-')) {
			const QRegularExpressionMatch match = singleOptionExpression.match(item);
			if (!match.hasMatch()) {
				throw MalformedOptionException("Malformed option: " + item);
			}
			QString items = match.captured("names");
			const QString last = items.remove(items.size() - 1);
			for (const QChar &option : items) {
				handleOption(option, QString(), false, nullptr);
			}
			handleOption(last, match.captured("value"), !match.captured("valuecont").isEmpty(), &it);
		} else {
			handleArguments({item});
		}
	}

	for (const Option &opt : options) {
		if (!result.options.contains(opt.names().first()) && opt.hasArgument() && !opt.defaultValue().isNull()) {
			result.options.insert(opt.names().first(), opt.defaultValue());
		}
	}

	return Result(result.options, result.arguments, result.commandChain, options, positionals);
}

void Parser::handle(const Result &result) const
{
	for (const QString &option : result.options().keys()) {
		const Option &opt = result.possibleOptions().value(option);
		if (opt.isEarlyExit()) {
			opt.call(result);
		}
	}

	for (const PositionalArgument &arg : result.possiblePositionals()) {
		if (!result.hasArgument(arg.name()) && !arg.isOptional()) {
			throw MissingPositionalArgumentException(QString("Missing required positional argument '%1'").arg(arg.name()));
		}
	}

	for (const QString &option : result.options().keys()) {
		const Option &opt = result.possibleOptions().value(option);
		if (!opt.allowedValues().isEmpty() && !opt.allowedValues().contains(result.value(option))) {
			throw InvalidOptionValueException(QString("The value to -%1%2 is not allowed; valid values: %3")
											  .arg(option.size() == 1 ? "-" : "").arg(option)
											  .arg(opt.allowedValues().toList().join(", ")));
		}
		if (!opt.isEarlyExit()) {
			opt.call(result);
		}
	}
	Command cmd = Command().add(*this); // the first item in the commandChain will be the Parser, so we need to make sure the initial item has it as a child
	for (const QString &command : result.commandChain()) {
		cmd = cmd.subcommands().value(command);
		cmd.call(result);
	}
}

template <>
bool Result::value<bool>(const QString &key) const
{
	const QString value = m_options.value(key);
	if (m_possibleOptions.value(key).argument().isNull()) {
		if (key.startsWith("no-") || key.startsWith("disable-")) {
			return !isSet(key);
		} else {
			return isSet(key);
		}
	} else if (value == "1" || value.toLower() == "on" || value.toLower() == "true") {
		return true;
	} else {
		return false;
	}
}

void Command::checkPositionals(const QVector<PositionalArgument> &arguments)
{
	if (arguments.size() > 1) {
		const QVector<PositionalArgument> exceptLast = arguments.mid(0, arguments.size()-1);

		// only the last may be multi
		for (const PositionalArgument &arg : exceptLast) {
			if (arg.isMulti()) {
				throw BuildException("Only the last positional argument may be multi");
			}
		}
		// if the last is multi, no arguments before it may be optional
		if (arguments.last().isMulti()) {
			for (const PositionalArgument &arg : exceptLast) {
				if (arg.isOptional()) {
					throw BuildException("May now have optional positional argument before multi positional argument");
				}
			}
		}
		// no more required allowed after the first optional
		bool haveHadOptional = false;
		for (const PositionalArgument &arg : arguments) {
			if (haveHadOptional && !arg.isOptional()) {
				throw BuildException("May not have required positional arguments after optional ones");
			} else if (arg.isOptional()) {
				haveHadOptional = true;
			}
		}
	}
}

QString detail::valueOf(const Option &option, const Result &result)
{
	return result.value(option.names().first());
}

CommandLineException::~CommandLineException() {}

}
}
}
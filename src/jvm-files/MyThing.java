public class MyThing {

	public String foo;
	public String bar;

	public MyThing(String foo, String bar) {

		this.foo = foo;
		this.bar = bar;
	}

	public static void main(String args[]) {

		for (int i = 0; i < 100; ++i) {

			MyThing myThing = new MyThing("foo", "bar");
		}

		System.out.println("Some things happened");
	}
}

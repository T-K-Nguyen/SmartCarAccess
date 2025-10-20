import 'package:smart_car_app/signup.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

class ForgotPassword extends StatefulWidget {
  const ForgotPassword({super.key});

  @override
  State<ForgotPassword> createState() => _ForgotPasswordState();
}

class _ForgotPasswordState extends State<ForgotPassword> {
  String email = "";
  TextEditingController mailcontroller = TextEditingController();

  final _formkey = GlobalKey<FormState>();

  resetPassword() async {
    // ensure email is trimmed and non-empty
    email = mailcontroller.text.trim();
    if (email.isEmpty) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(
        "Please enter your email address.",
        style: TextStyle(fontSize: 16.0),
      )));
      return;
    }

    try {
      await FirebaseAuth.instance.sendPasswordResetEmail(email: email);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(
        "Password Reset Email has been sent!",
        style: TextStyle(fontSize: 16.0),
      )));
    } on FirebaseAuthException catch (e) {
      if (!mounted) return;
      if (e.code == "user-not-found") {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
            content: Text(
          "No user found for that email.",
          style: TextStyle(fontSize: 16.0),
        )));
      } else {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
            content: Text(
          "Error: ${e.message}",
          style: TextStyle(fontSize: 16.0),
        )));
      }
    }
  }

  @override
  void dispose() {
    mailcontroller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.white,
      body: Column(
        children: [
            SizedBox(
              height: 20.0,
            ),
            // Illustration for password recovery
            Center(
              child: SizedBox(
                width: MediaQuery.of(context).size.width,
                child: Image.asset(
                  'images/forgot.jpg',
                  fit: BoxFit.cover,
                ),
              ),
            ),
            SizedBox(
              height: 12.0,
            ),
            Container(
              alignment: Alignment.topCenter,
              child: Text(
                "Password Recovery",
                style: TextStyle(
                    color: Color(0xFF273671),
                    fontSize: 30.0,
                    fontWeight: FontWeight.bold),
              ),
            ),
            SizedBox(height: 20.0),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20.0, vertical: 2.0),
              child: Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  "Enter Your Email",
                  style: TextStyle(
                      color: Color(0xFF273671),
                      fontSize: 20.0,
                      fontWeight: FontWeight.bold),
                ),
              ),
            ),
            Expanded(
              child: Form(
                key: _formkey,
                child: Padding(
                  padding: EdgeInsets.symmetric(horizontal: 20.0, vertical: 0.0),
                      child: ListView(
                        children: [
                          Container(
                            padding: EdgeInsets.only(left: 10.0),
                            decoration: BoxDecoration(
                              border:
                                  Border.all(color: Colors.black, width: 2.0),
                              borderRadius: BorderRadius.circular(30),
                            ),
                            child: TextFormField(
                              validator: (value) {
                                if (value == null || value.isEmpty) {
                                  return 'Please Enter Email';
                                }
                                return null;
                              },
                              controller: mailcontroller,
                              style: TextStyle(color: Colors.black),
                              decoration: InputDecoration(
                                  hintText: "Email",
                                  hintStyle: TextStyle(
                                      fontSize: 18.0, color: Colors.black54),
                                  prefixIcon: Icon(
                                    Icons.person,
                                    color: Colors.black54,
                                    size: 30.0,
                                  ),
                                  border: InputBorder.none),
                            ),
                          ),
                          SizedBox(
                            height: 30.0,
                          ),
                          GestureDetector(
                            onTap: () {
                              if(_formkey.currentState!.validate()){
                                setState(() {
                                  email=mailcontroller.text;
                                });
                                resetPassword();
                              }
                            },
                            child: Container(
                              width: 140,
                              padding: EdgeInsets.all(10),
                              decoration: BoxDecoration(
                                  color: Colors.white,
                                  borderRadius: BorderRadius.circular(10)),
                              child: Center(
                                child: Text(
                                  "Send Email",
                                  style: TextStyle(
                                      color: Color(0xFF273671),
                                      fontSize: 18.0,
                                      fontWeight: FontWeight.bold),
                                ),
                              ),
                            ),
                          ),
                          SizedBox(
                            height: 50.0,
                          ),
                          Row(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: [
                              Text(
                                "Don't have an account?",
                                style: TextStyle(
                                    fontSize: 18.0, color: Colors.black54),
                              ),
                              SizedBox(
                                width: 5.0,
                              ),
                              GestureDetector(
                                onTap: () {
                                  Navigator.push(
                                      context,
                                      MaterialPageRoute(
                                          builder: (context) => SignUp()));
                                },
                                child: Text(
                                  "Sign In",
                                  style: TextStyle(
                                      color: Color(0xFF273671),
                                      fontSize: 20.0,
                                      fontWeight: FontWeight.w500),
                                ),
                              )
                            ],
                          )
                        ],
                      ),
                    ))),
          ],
      ),
    );
  }
}